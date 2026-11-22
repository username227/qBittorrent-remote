// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QComboBox>
#include <QWidget>

class QLineEdit;
class QToolButton;

// Remote adaptation of qBittorrent's FileSystemPathComboEdit.
// Preserve its native combo + small folder-icon button arrangement while
// delegating browsing/translation to the selected server's path mappings.
// Unlike upstream, this has no local-engine Path dependency.
class RemoteFileSystemPathComboEdit final : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteFileSystemPathComboEdit(QWidget *parent = nullptr);

    QComboBox *comboBox() const { return m_combo; }
    QLineEdit *lineEdit() const;
    void setEditable(bool editable);
    void setInsertPolicy(QComboBox::InsertPolicy policy);
    void setCurrentIndex(int index);
    void addItem(const QString &text);
    void setEditText(const QString &text);
    QString currentText() const;

signals:
    void browseRequested();

private:
    QComboBox *m_combo = nullptr;
    QToolButton *m_browse = nullptr;
};
