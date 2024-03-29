#include "zipdownloader.hpp"

#include "miniz.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QDebug>

namespace zdl
{
namespace
{
template <typename OnSuccess, typename OnProgress, typename OnError>
class HTTPGet final : public QNetworkAccessManager
{
public:
  explicit HTTPGet(QUrl url, OnSuccess on_success, OnProgress on_progress, OnError on_error) noexcept
      : m_callback{std::move(on_success)},
        m_progress{std::move(on_progress)},
        m_error{std::move(on_error)}
{
    connect(this, &QNetworkAccessManager::finished,
            this, [this](QNetworkReply* reply) {
          if(reply->error())
          {
              qDebug() << reply->errorString();
              m_error();
          }
          else
          {
              m_callback(reply->readAll());
          }

          reply->deleteLater();
          this->deleteLater();
    });

    QNetworkRequest req{std::move(url)};
    req.setRawHeader("User-Agent", "curl/7.35.0");

    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
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
    connect(reply, &QNetworkReply::redirected,
            reply, &QNetworkReply::redirectAllowed);
    connect(reply, &QNetworkReply::downloadProgress,
            this, m_progress);
}

private:
  OnSuccess m_callback;
  OnProgress m_progress;
  OnError m_error;
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

QString slash_path(const QString& str)
{
  return {};
}

QString relative_path(const QString& base, const QString& filename)
{
  return filename;
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

// Mostly based on https://github.com/tessel/miniz/blob/master/example2.c
std::vector<QString> unzip(const QByteArray& zipFile, const QString& path)
{
  std::vector<QString> files;

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

  // Get root folder
  QString lastDir = "";
  QString base
      = slash_path(get_path(file_stat.m_filename)); // path delim on end

  // Get and print information about each file in the archive.
  for (int i = 0; i < fileCount; i++)
  {
    if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
      continue;
    if (mz_zip_reader_is_file_a_directory(&zip_archive, i))
      continue; // skip directories for now
    QString fileName
        = relative_path(base, file_stat.m_filename); // make path relative
    QString destFile = combine_path(path, fileName); // make full dest path
    auto newDir = get_path(fileName);                // get the file's path
    if (newDir != lastDir)
    {
      if (!make_folder(combine_path(path, newDir))) // creates the directory
      {
        return files;
      }
    }

    if (mz_zip_reader_extract_to_file(&zip_archive, i, destFile.toUtf8(), 0))
    {
      files.emplace_back(destFile);
    }
  }

  // Close the archive, freeing any resources it was using
  mz_zip_reader_end(&zip_archive);

  return files;
}
}

void download_and_extract(
    const QUrl& url,
    const QString& destination,
    const success_callback& success_cb,
    const progress_callback& progress_cb,
    const error_callback& error_cb)
{
    new HTTPGet{
        url,
        [=] (const QByteArray& data) { success_cb(unzip(data, destination)); },
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
