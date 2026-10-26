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

//! Extract every file of the archive under @p destination, at the paths the
//! archive stores. Returns the files and links written.
//!
//! What a Unix archive records is restored: permissions (the owner always keeps
//! read and write), and symbolic links, created once every file is in place.
//! Where links cannot be created, as on Windows without the privilege, each one
//! becomes a copy of the file it points to.
//!
//! An archive holding an entry which would land outside @p destination, or a
//! link pointing outside it, is refused whole: nothing is written and @p error
//! says which entry it was. One whose files cannot all be written (a full disk)
//! fails too: an empty result and @p error, with what was written left in place.
ZIPDOWNLOADER_EXPORT
std::vector<QString> unzip_all_files_to_folder(
    const QByteArray& zipFile, const QString& destination, QString& error);

//! Same, reading the archive from the file @p zipFile instead of memory.
ZIPDOWNLOADER_EXPORT
std::vector<QString> unzip_all_files_to_folder(
    const QString& zipFile, const QString& destination, QString& error);
}
