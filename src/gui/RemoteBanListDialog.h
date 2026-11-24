// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QStringList>
#include <memory>

class QStringListModel;
namespace Ui { class BanListOptionsDialog; }

// Native upstream layout with staged, remote-server list editing.
class RemoteBanListDialog final : public QDialog
{
    Q_OBJECT
public:
    RemoteBanListDialog(const QString &entries, bool rangesSupported, QWidget *parent = nullptr);
    ~RemoteBanListDialog() override;
    QString entries() const;
    static QString canonicalEntry(const QString &input, bool rangesSupported);
    static bool validList(const QString &entries, bool rangesSupported);

private:
    std::unique_ptr<Ui::BanListOptionsDialog> m_ui;
    QStringListModel *m_model = nullptr;
    bool m_rangesSupported;
};
