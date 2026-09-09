#pragma once
#include <zipdownloader_export.h>
#include <QString>
#include <QUrl>
#include <QByteArray>
#include <functional>
#include <utility>
#include <vector>

namespace zdl
{
// Argument is the list of extracted files
using success_callback = std::function<void(std::vector<QString>)>;
using progress_callback = std::function<void(qint64, qint64)>;
//! Argument is what went wrong, in the words of the network stack: without it
//! a failed download is indistinguishable from any other, which is exactly the
//! position a user hitting one is in.
using error_callback = std::function<void(const QString&)>;

//! No documentation. Fuck the police.
ZIPDOWNLOADER_EXPORT
void download_and_extract(
    const QUrl& url,
    const QString& destination,
    const success_callback& success_cb,
    const progress_callback& progress_cb,
    const error_callback& error_cb);

ZIPDOWNLOADER_EXPORT
std::vector<std::pair<QString, QByteArray>> unzip_all_files_to_memory(const QByteArray& zipFile);
}
