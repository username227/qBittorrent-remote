// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"
#include <QDialog>
#include <QJsonObject>
#include <QSet>
#include <memory>

namespace Ui { class TorrentCategoryDialog; }
class RemoteFileSystemPathComboEdit;

// Native upstream category form, with server paths and Web API parameters.
class RemoteCategoryDialog final : public QDialog
{
    Q_OBJECT
public:
    RemoteCategoryDialog(const QString &name, bool edit, const QJsonObject &definition,
                         const QJsonObject &capabilities, const QSet<QString> &existing,
                         const QList<Remote::PathMapping> &mappings, QWidget *parent = nullptr);
    ~RemoteCategoryDialog() override;
    QString categoryName() const;
    QJsonObject fields() const;
protected:
    void accept() override;
private:
    void browse(RemoteFileSystemPathComboEdit *field);
    std::unique_ptr<Ui::TorrentCategoryDialog> m_ui;
    bool m_edit;
    QJsonObject m_original;
    QSet<QString> m_existing;
    QList<Remote::PathMapping> m_mappings;
    QString m_lastDownloadPath;
};
