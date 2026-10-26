#include "zipdownloader.hpp"

#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
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
      : m_callback{std::move(on_success)},
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
              m_file->close();
              m_callback(m_file->fileName());
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
#endif

    auto reply = get(req);
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
//! False when it would land outside: an absolute path, a Windows drive or UNC
//! prefix, a backslash separator, or a ".." climbing above the root.
bool relative_path(const QString& filename, QString& out)
{
  if (filename.startsWith('/') || filename.contains('\\'))
    return false;
  if (filename.size() > 1 && filename[1] == ':')
    return false;

  QStringList parts;
  for (const QString& part : filename.split('/', Qt::SkipEmptyParts))
  {
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
//! uninstalled later.
QFileDevice::Permissions to_permissions(mz_uint32 mode)
{
  QFileDevice::Permissions p = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
  if (mode & 0100) p |= QFileDevice::ExeOwner;
  if (mode & 0040) p |= QFileDevice::ReadGroup;
  if (mode & 0020) p |= QFileDevice::WriteGroup;
  if (mode & 0010) p |= QFileDevice::ExeGroup;
  if (mode & 0004) p |= QFileDevice::ReadOther;
  if (mode & 0002) p |= QFileDevice::WriteOther;
  if (mode & 0001) p |= QFileDevice::ExeOther;
  return p;
}

struct link_entry
{
  QString path;   // where the link goes, relative to the destination
  QString target; // what it points to, relative to the link's folder
};

//! Creates the archive's symbolic links once every file is in place.
//!
//! Where links cannot be created (Windows without the privilege), each one is
//! replaced by a copy of what it points to: the package then holds the same
//! names, which is what its users look the files up by. Links to links are
//! copied in the order that resolves them; a link to a folder is not copied.
bool create_links(
    const QString& root, const std::vector<link_entry>& links,
    std::vector<QString>& files, QString& error)
{
  std::vector<link_entry> pending;
  for (const auto& link : links)
  {
    const QString dest = combine_path(root, link.path);
    QFile::remove(dest);
#if !defined(_WIN32)
    if (::symlink(link.target.toUtf8().constData(), dest.toUtf8().constData()) == 0)
    {
      files.emplace_back(dest);
      continue;
    }
#endif
    pending.push_back(link);
  }

  for (bool progress = true; progress && !pending.empty();)
  {
    progress = false;
    for (auto it = pending.begin(); it != pending.end();)
    {
      const QString dest = combine_path(root, it->path);
      const QString source = combine_path(QFileInfo{dest}.absolutePath(), it->target);
      if (QFileInfo{source}.isFile() && QFile::copy(source, dest))
      {
        files.emplace_back(dest);
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
  {
    error = QStringLiteral("Could not create the link %1 -> %2")
                .arg(pending.front().path, pending.front().target);
    return false;
  }
  return true;
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

    const QString link_target
        = QString::fromUtf8(target, (int)file_stat.m_uncomp_size);
    const QString folder = get_path(names[i]);
    QString resolved;
    if (link_target.isEmpty()
        || !relative_path(folder.isEmpty() ? link_target : folder + '/' + link_target, resolved)
        || resolved.isEmpty())
    {
      error = QStringLiteral("The archive holds a link which points outside the "
                             "destination folder: %1 -> %2")
                  .arg(name, link_target);
      return {};
    }
    links.push_back({names[i], link_target});
  }

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
      if (!make_folder(combine_path(path, newDir))) // creates the directory
      {
        error = QStringLiteral("Could not create the folder %1").arg(combine_path(path, newDir));
        return {};
      }
      lastDir = newDir;
    }

    // A file that does not make it out (a full disk, typically) fails the whole
    // extraction: a package missing some of its files is not an installed package.
    if (!mz_zip_reader_extract_to_file(&zip_archive, i, destFile.toUtf8(), 0))
    {
      error = QStringLiteral("Could not extract %1 to %2: %3")
                  .arg(fileName, destFile,
                       QString::fromUtf8(mz_zip_get_error_string(mz_zip_get_last_error(&zip_archive))));
      return {};
    }

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
