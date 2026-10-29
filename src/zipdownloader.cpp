#include "zipdownloader.hpp"

#include "miniz.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QTemporaryFile>
#include <QUrl>
#include <QDebug>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace zdl
{
namespace
{
//! Downloads @p url into a temporary file next to @p destination rather than in
//! memory: support packages weigh more than a gigabyte, and holding one in RAM
//! next to its extracted copy is how a download takes the machine down with it.
template <typename OnSuccess, typename OnProgress, typename OnError>
class HTTPGet final : public QNetworkAccessManager
{
public:
  explicit HTTPGet(
      QUrl url, const QString& destination, OnSuccess on_success,
      OnProgress on_progress, OnError on_error) noexcept
      // Owned by the application: quitting mid-download destroys us, and the
      // temporary file with us, instead of leaving a gigabyte behind.
      : QNetworkAccessManager{QCoreApplication::instance()},
        m_callback{std::move(on_success)},
        m_progress{std::move(on_progress)},
        m_error{std::move(on_error)}
  {
    // Beside the destination, not in it: the destination's contents are what the
    // caller inspects afterwards, and the system temp folder may be a small tmpfs.
    QDir{}.mkpath(QFileInfo{destination}.absolutePath());
    m_file = new QTemporaryFile{
        QFileInfo{destination}.absolutePath() + QStringLiteral("/.zdl-download-XXXXXX.zip"),
        this};
    if(!m_file->open())
    {
      const QString msg = QStringLiteral("Could not create a temporary file for the download in %1: %2")
                              .arg(QFileInfo{destination}.absolutePath(), m_file->errorString());
      // Reported asynchronously like every other outcome: callers do not expect
      // their error callback to run before download_and_extract returns.
      QMetaObject::invokeMethod(this, [this, msg] {
        m_error(msg);
        deleteLater();
      }, Qt::QueuedConnection);
      return;
    }

    connect(this, &QNetworkAccessManager::finished,
            this, [this](QNetworkReply* reply) {
          QString msg = m_writeError;
          if(msg.isEmpty() && reply->error())
          {
              // Say what happened: a download that fails two seconds in and
              // reports nothing leaves the user, and us, with nothing to go on.
              msg = reply->errorString();
              const auto status
                  = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
              if(status.isValid())
                msg += QStringLiteral(" (HTTP %1)").arg(status.toInt());
              msg += QStringLiteral("\n") + reply->url().toString();
          }
          if(msg.isEmpty())
            msg = write(reply->readAll());

          if(msg.isEmpty())
          {
              // Released before it is handed on, not just closed: on Windows a
              // closed QTemporaryFile keeps its handle, and miniz opens the
              // archive with _wfopen_s, which refuses to share it with a writer.
              const QString fileName = m_file->fileName();
              m_file->setAutoRemove(false);
              delete m_file;
              m_file = nullptr;
              m_callback(fileName);
              QFile::remove(fileName);
          }
          else
          {
              qDebug() << "zipdownloader:" << msg;
              m_error(msg);
          }

          reply->deleteLater();
          this->deleteLater();
    });

    QNetworkRequest req{std::move(url)};
    // Not "curl/7.35.0": a corporate proxy or a CDN that filters on the agent
    // turns that into a refusal within seconds, on that machine only.
    req.setRawHeader("User-Agent", "zipdownloader");

    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    // Follow redirects rather than asking to approve each one: a release asset
    // served from another host redirects on every request, and the approval
    // has to be connected before the reply can emit it.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#elif QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    req.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
    req.setAttribute(QNetworkRequest::SpdyAllowedAttribute, true);
#else
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    // A connection which stops sending is an error, not a download that never
    // finishes. This is the time without data, not the length of the download.
    req.setTransferTimeout(60000);
#endif

    auto reply = get(req);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
      if(!m_writeError.isEmpty())
        return;
      m_writeError = write(reply->readAll());
      if(!m_writeError.isEmpty())
        reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress,
            this, m_progress);
}

  ~HTTPGet() override
  {
    // Destroyed with the application mid-download: the replies aborted by the
    // base destructor must not reach callbacks whose objects may be gone.
    disconnect(this, nullptr, this, nullptr);
    if(m_reply)
      m_reply->disconnect(this);
  }

private:
  //! Appends to the temporary file; an error message when the disk refuses.
  QString write(const QByteArray& data)
  {
    if(m_file->write(data) != data.size())
      return QStringLiteral("Could not write the download to %1: %2")
          .arg(m_file->fileName(), m_file->errorString());
    return {};
  }

  OnSuccess m_callback;
  OnProgress m_progress;
  OnError m_error;
  QTemporaryFile* m_file{};
  QPointer<QNetworkReply> m_reply;
  QString m_writeError;
};

QString get_path(const QString& str)
{
  auto idx = str.lastIndexOf('/');
  if (idx != -1)
  {
    return str.mid(0, idx);
  }
  return "";
}

//! Where an archive entry lands under the destination folder, with the "."
//! and ".." components resolved.
//!
#if defined(_WIN32)
//! NUL, COM1, CON.txt...: Windows opens the device, whatever the folder.
bool is_device_name(const QString& part)
{
  const QString base = part.section('.', 0, 0).trimmed().toUpper();
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
    return true;
  return base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT"))
         && base[3] >= '1' && base[3] <= '9';
}
#endif

//! False when it would land outside: an absolute path, a Windows drive or UNC
//! prefix, a backslash separator, or a ".." climbing above the root. On Windows
//! also a ':' (NTFS reads "name:stream" as a stream of "name") or a device name.
bool relative_path(const QString& filename, QString& out)
{
  if (filename.startsWith('/') || filename.contains('\\'))
    return false;
  if (filename.size() > 1 && filename[1] == ':')
    return false;

  QStringList parts;
  for (const QString& part : filename.split('/', Qt::SkipEmptyParts))
  {
#if defined(_WIN32)
    if (part.contains(':') || is_device_name(part))
      return false;
#endif
    if (part == ".")
      continue;
    else if (part != "..")
      parts.push_back(part);
    else if (parts.isEmpty())
      return false;
    else
      parts.removeLast();
  }

  out = parts.join('/');
  return true;
}

QString combine_path(const QString& path, const QString& filename)
{
  return path + "/" + filename;
}

bool make_folder(const QString& str)
{
  QDir d;
  return d.mkpath(str);
}

// Unix archivers (Info-ZIP's zip, Python's zipfile, libarchive...) store the
// entry's st_mode in the upper half of the external attributes, and say so
// through the "version made by" host byte.
constexpr mz_uint32 host_unix = 3;
constexpr mz_uint32 host_macos = 19;
constexpr mz_uint32 mode_type_mask = 0170000;
constexpr mz_uint32 mode_symlink = 0120000;

//! The entry's Unix st_mode, or 0 when the archive was not made on a Unix and
//! carries none.
mz_uint32 unix_mode(const mz_zip_archive_file_stat& stat)
{
  const mz_uint32 host = stat.m_version_made_by >> 8;
  if (host != host_unix && host != host_macos)
    return 0;
  return stat.m_external_attr >> 16;
}

//! The permission bits of @p mode as Qt permissions. The owner keeps read and
//! write whatever the archive says, so that the package can be updated or
//! uninstalled later. Nobody else gets to write: a library another user can
//! replace is a library they run code in this process with.
QFileDevice::Permissions to_permissions(mz_uint32 mode)
{
  QFileDevice::Permissions p = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
  if (mode & 0100) p |= QFileDevice::ExeOwner;
  if (mode & 0040) p |= QFileDevice::ReadGroup;
  if (mode & 0010) p |= QFileDevice::ExeGroup;
  if (mode & 0004) p |= QFileDevice::ReadOther;
  if (mode & 0001) p |= QFileDevice::ExeOther;
  return p;
}

struct link_entry
{
  QString path;   // where the link goes, relative to the destination
  QString target; // what it points to, relative to the link's folder
};

//! The names of the archive's links, as compared on a case-insensitive
//! filesystem too: "Lib" and "lib/x" are the same folder on macOS and Windows.
using link_set = QSet<QString>;

//! Whether reaching @p path, relative to the destination, goes through one of
//! the archive's links: the link may point anywhere inside, and what is under it
//! is then somewhere else than the archive says.
bool through_link(const QString& path, const link_set& links)
{
  for (int i = path.indexOf('/'); i != -1; i = path.indexOf('/', i + 1))
    if (links.contains(path.left(i).toCaseFolded()))
      return true;
  return false;
}

//! Whether the link at @p path, to @p target, points inside the destination.
//!
//! The filesystem resolves a target one component at a time, following links as
//! it meets them: with "u -> .." in the archive, "u/../../x" climbs from where u
//! points, not from u's folder. So the target may only name one of the
//! archive's links as its last component (that link is checked on its own),
//! never on its way. Then resolving it as text is resolving it as the
//! filesystem will.
bool link_inside(const QString& path, const QString& target, const link_set& links)
{
  const QString folder = get_path(path);
  QString resolved;
  if (target.isEmpty()
      || !relative_path(folder.isEmpty() ? target : folder + '/' + target, resolved)
      || resolved.isEmpty())
    return false;

  QStringList parts = folder.split('/', Qt::SkipEmptyParts);
  const QStringList steps = target.split('/', Qt::SkipEmptyParts);
  for (int i = 0; i < steps.size(); i++)
  {
    if (steps[i] == ".")
      continue;
    if (steps[i] == "..")
    {
      parts.removeLast(); // relative_path said it does not climb out
      continue;
    }
    parts.push_back(steps[i]);
    if (i + 1 < steps.size() && links.contains(parts.join('/').toCaseFolded()))
      return false;
  }
  return true;
}

//! @p path as the filesystem resolves it now is @p root or under it. @p root
//! is canonical.
bool physically_under(const QString& root, const QString& path)
{
  const QString canonical = QFileInfo{path}.canonicalFilePath();
  return !canonical.isEmpty() && (canonical == root || canonical.startsWith(root + '/'));
}

//! Creates the archive's symbolic links once every file is in place.
//!
//! Where links cannot be created (Windows without the privilege), each one is
//! replaced by a copy of what it points to: the package then holds the same
//! names, which is what its users look the files up by. Links to links are
//! copied in the order that resolves them; a link to a folder is not copied.
//!
//! What the archive's links point to was checked before anything was written;
//! where they point once created is checked again against the filesystem, which
//! is what loading through them will see.
bool create_links(
    const QString& root, const std::vector<link_entry>& links,
    std::vector<QString>& files, QString& error)
{
  const QString canonicalRoot = QFileInfo{root}.canonicalFilePath();
  auto fail = [&](const QString& message) {
    error = message;
    return false;
  };

  std::vector<link_entry> pending;
  std::vector<QString> created;
  for (const auto& link : links)
  {
    const QString dest = combine_path(root, link.path);
    // A link may be the only entry of its folder.
    const QString folder = QFileInfo{dest}.absolutePath();
    if (!make_folder(folder) || !physically_under(canonicalRoot, folder))
      return fail(QStringLiteral("Could not create the folder %1").arg(folder));

    QFile::remove(dest);
#if !defined(_WIN32)
    if (::symlink(link.target.toUtf8().constData(), dest.toUtf8().constData()) == 0)
    {
      created.push_back(dest);
      continue;
    }
#endif
    pending.push_back(link);
  }

  for (const QString& dest : created)
  {
    // Dangling, cyclic or outside: a package is not installed with that in it,
    // whichever platform it is installed on.
    if (!physically_under(canonicalRoot, dest))
    {
      for (const QString& link : created)
        QFile::remove(link);
      return fail(QStringLiteral("The archive holds a link which does not resolve "
                                 "inside the destination folder: %1")
                      .arg(QDir{root}.relativeFilePath(dest)));
    }
    files.push_back(dest);
  }

  for (bool progress = true; progress && !pending.empty();)
  {
    progress = false;
    for (auto it = pending.begin(); it != pending.end();)
    {
      const QString dest = combine_path(root, it->path);
      const QString source = combine_path(QFileInfo{dest}.absolutePath(), it->target);
      const QFileInfo info{source};
      if (info.isDir() && physically_under(canonicalRoot, source))
      {
        qDebug() << "zipdownloader: not copying the link to a folder" << it->path;
        it = pending.erase(it);
        progress = true;
      }
      else if (info.isFile() && physically_under(canonicalRoot, source)
               && QFile::copy(source, dest))
      {
        files.push_back(dest);
        it = pending.erase(it);
        progress = true;
      }
      else
      {
        ++it;
      }
    }
  }

  for (const auto& link : pending)
    qDebug() << "zipdownloader: could not create" << link.path << "->" << link.target;
  if (!pending.empty())
    return fail(QStringLiteral("Could not create the link %1 -> %2")
                    .arg(pending.front().path, pending.front().target));
  return true;
}

//! miniz's write callback, appending to the open QFile @p opaque. miniz writes
//! each entry in order; anything else is reported as a failed write.
size_t write_to_file(void* opaque, mz_uint64 offset, const void* data, size_t size)
{
  auto& file = *static_cast<QFile*>(opaque);
  if (file.pos() != (qint64)offset)
    return 0;
  return file.write(static_cast<const char*>(data), (qint64)size) == (qint64)size ? size : 0;
}

//! Extracts every entry of an initialized archive under @p path: regular files
//! with the permissions a Unix archive records, then symbolic links.
std::vector<QString> extract_all(mz_zip_archive& zip_archive, const QString& path, QString& error)
{
  std::vector<QString> files;

  const int fileCount = (int)mz_zip_reader_get_num_files(&zip_archive);
  if (fileCount == 0)
    return files;

  // Every entry is checked before anything is written: a package with one
  // entry pointing out of the destination is not one to install half of.
  // That includes where symbolic links point.
  std::vector<QString> names(fileCount);
  std::vector<mz_uint32> modes(fileCount);
  std::vector<link_entry> links;
  link_set link_names;
  for (int i = 0; i < fileCount; i++)
  {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
      continue;

    const QString name = QString::fromUtf8(file_stat.m_filename);
    if (!relative_path(name, names[i]))
    {
      error = QStringLiteral("The archive holds an entry which would be written "
                             "outside the destination folder: %1")
                  .arg(name);
      return {};
    }

    modes[i] = unix_mode(file_stat);
    if ((modes[i] & mode_type_mask) != mode_symlink || names[i].isEmpty())
      continue;

    // A link's data is its target. Anything longer than a path is not a link.
    char target[4096];
    if (file_stat.m_uncomp_size >= sizeof(target)
        || !mz_zip_reader_extract_to_mem(
            &zip_archive, i, target, (size_t)file_stat.m_uncomp_size, 0))
    {
      error = QStringLiteral("The archive holds an unreadable link: %1").arg(name);
      return {};
    }

    links.push_back({names[i], QString::fromUtf8(target, (int)file_stat.m_uncomp_size)});
    link_names.insert(names[i].toCaseFolded());
  }

  for (int i = 0; i < fileCount; i++)
  {
    if (through_link(names[i], link_names))
    {
      error = QStringLiteral("The archive holds an entry under one of its links: %1")
                  .arg(names[i]);
      return {};
    }
  }
  for (const auto& link : links)
  {
    if (!link_inside(link.path, link.target, link_names))
    {
      error = QStringLiteral("The archive holds a link which points outside the "
                             "destination folder: %1 -> %2")
                  .arg(link.path, link.target);
      return {};
    }
  }

  if (!make_folder(path))
  {
    error = QStringLiteral("Could not create the folder %1").arg(path);
    return {};
  }
  const QString canonicalRoot = QFileInfo{path}.canonicalFilePath();

  QString lastDir;
  for (int i = 0; i < fileCount; i++)
  {
    const QString& fileName = names[i];
    if (fileName.isEmpty())
      continue;
    if (mz_zip_reader_is_file_a_directory(&zip_archive, i))
      continue; // created along with the files they hold
    if ((modes[i] & mode_type_mask) == mode_symlink)
      continue; // created once every file exists

    QString destFile = combine_path(path, fileName); // make full dest path
    auto newDir = get_path(fileName);                // get the file's path
    if (newDir != lastDir)
    {
      // The archive cannot lead out of the destination; a destination which is
      // not empty can, with links of its own.
      const QString dir = combine_path(path, newDir);
      if (!make_folder(dir) || !physically_under(canonicalRoot, dir))
      {
        error = QStringLiteral("Could not create the folder %1").arg(dir);
        return {};
      }
      lastDir = newDir;
    }

    // Replaced, not written through.
    if (QFileInfo{destFile}.isSymLink())
      QFile::remove(destFile);

    // A file that does not make it out (a full disk, typically) fails the whole
    // extraction: a package missing some of its files is not an installed package.
    // Written through QFile rather than miniz's own fopen: on Windows only the
    // former reaches past MAX_PATH, and packages nest deep enough to need it.
    QFile out{destFile};
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      error = QStringLiteral("Could not extract %1 to %2: %3")
                  .arg(fileName, destFile, out.errorString());
      return {};
    }
    if (!mz_zip_reader_extract_to_callback(&zip_archive, i, write_to_file, &out, 0))
    {
      const QString reason = out.error() != QFileDevice::NoError
          ? out.errorString()
          : QString::fromUtf8(mz_zip_get_error_string(mz_zip_get_last_error(&zip_archive)));
      error = QStringLiteral("Could not extract %1 to %2: %3")
                  .arg(fileName, destFile, reason);
      return {};
    }

    // What mz_zip_reader_extract_to_file did on top of the bytes.
    if (mz_zip_archive_file_stat file_stat; mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
      out.setFileTime(QDateTime::fromSecsSinceEpoch(file_stat.m_time),
                      QFileDevice::FileModificationTime);
    out.close();

    if (modes[i] & 0777)
      QFile::setPermissions(destFile, to_permissions(modes[i]));
    files.emplace_back(destFile);
  }

  if (!create_links(path, links, files, error))
    return {};

  return files;
}
}

std::vector<QString> unzip_all_files_to_folder(
    const QByteArray& zipFile, const QString& path, QString& error)
{
  mz_zip_archive zip_archive;
  memset(&zip_archive, 0, sizeof(zip_archive));

  if (!mz_zip_reader_init_mem(&zip_archive, zipFile.data(), zipFile.size(), 0))
    return {};

  auto files = extract_all(zip_archive, path, error);
  mz_zip_reader_end(&zip_archive);
  return files;
}

std::vector<QString> unzip_all_files_to_folder(
    const QString& zipFile, const QString& path, QString& error)
{
  mz_zip_archive zip_archive;
  memset(&zip_archive, 0, sizeof(zip_archive));

  if (!mz_zip_reader_init_file(&zip_archive, zipFile.toUtf8().constData(), 0))
    return {};

  auto files = extract_all(zip_archive, path, error);
  mz_zip_reader_end(&zip_archive);
  return files;
}

void download_and_extract(
    const QUrl& url,
    const QString& destination,
    const success_callback& success_cb,
    const progress_callback& progress_cb,
    const error_callback& error_cb)
{
    new HTTPGet{
        url, destination,
        [=] (const QString& zipFile) {
          QString error;
          auto files = unzip_all_files_to_folder(zipFile, destination, error);
          if(files.empty())
          {
            // What came back is not a zip we can read: an HTML error page, a
            // truncated body, an archive we cannot open. Reporting that as a
            // success with nothing in it is how a download "does nothing".
            if(error.isEmpty())
              error = QStringLiteral(
                          "The downloaded archive could not be read (%1 bytes).")
                          .arg(QFileInfo{zipFile}.size());
            qDebug() << "zipdownloader:" << error;
            error_cb(error);
            return;
          }
          success_cb(std::move(files));
        },
        progress_cb,
        error_cb
    };
}

std::vector<std::pair<QString, QByteArray>> unzip_all_files_to_memory(const QByteArray& zipFile)
{
  std::vector<std::pair<QString, QByteArray>> files;

  mz_zip_archive zip_archive;
  memset(&zip_archive, 0, sizeof(zip_archive));

  auto status = mz_zip_reader_init_mem(
        &zip_archive, zipFile.data(), zipFile.size(), 0);
  if (!status)
    return files;
  int fileCount = (int)mz_zip_reader_get_num_files(&zip_archive);
  if (fileCount == 0)
  {
    mz_zip_reader_end(&zip_archive);
    return files;
  }
  mz_zip_archive_file_stat file_stat;
  if (!mz_zip_reader_file_stat(&zip_archive, 0, &file_stat))
  {
    mz_zip_reader_end(&zip_archive);
    return files;
  }

  // Get and print information about each file in the archive.
  for (int i = 0; i < fileCount; i++)
  {
    if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
      continue;
    if (mz_zip_reader_is_file_a_directory(&zip_archive, i))
      continue; // skip directories for now
    if(file_stat.m_uncomp_size > std::numeric_limits<int32_t>::max())
      continue;

    QByteArray arr{int(file_stat.m_uncomp_size), Qt::Uninitialized};
    if (mz_zip_reader_extract_to_mem(&zip_archive, i, arr.data(), arr.size(), 0)) {
      files.emplace_back(QString::fromUtf8(file_stat.m_filename), std::move(arr));
    }
  }

  // Close the archive, freeing any resources it was using
  mz_zip_reader_end(&zip_archive);

  return files;
}

}
