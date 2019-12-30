#pragma once
#include <zipdownloader_export.h>
#include <QUrl>
#include <functional>

namespace zdl
{
// Argument is the list of extracted files
using success_callback = std::function<void(std::vector<QString>)>;
using error_callback = std::function<void()>;

//! No documentation. Fuck the police.
ZIPDOWNLOADER_EXPORT
void download_and_extract(
    const QUrl& url,
    const QString& destination,
    const success_callback& success_cb,
    const error_callback& error_cb);
}
