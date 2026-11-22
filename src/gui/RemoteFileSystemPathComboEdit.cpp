// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteFileSystemPathComboEdit.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

RemoteFileSystemPathComboEdit::RemoteFileSystemPathComboEdit(QWidget *parent)
    : QWidget(parent)
    , m_combo(new QComboBox(this))
    , m_browse(new QToolButton(this))
{
    // Matches the upstream FileSystemPathComboEdit's native, tight layout.
    // There must be no extra QGroupBox row or full-size "Browse…" button.
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_combo->setEditable(true);
    m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_browse->setObjectName(QStringLiteral("browseFolderButton"));
    m_browse->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_browse->setToolTip(tr("Choose a locally mounted NAS folder"));
    m_browse->setAutoRaise(false);
    m_browse->setFixedSize(qMax(26, m_combo->sizeHint().height()),
                           m_combo->sizeHint().height());

    layout->addWidget(m_combo, 1);
    layout->addWidget(m_browse);
    connect(m_browse, &QToolButton::clicked,
            this, &RemoteFileSystemPathComboEdit::browseRequested);
}

QLineEdit *RemoteFileSystemPathComboEdit::lineEdit() const
{
    return m_combo->lineEdit();
}
void RemoteFileSystemPathComboEdit::setEditable(bool editable)
{
    m_combo->setEditable(editable);
}
void RemoteFileSystemPathComboEdit::setInsertPolicy(QComboBox::InsertPolicy policy)
{
    m_combo->setInsertPolicy(policy);
}
void RemoteFileSystemPathComboEdit::setCurrentIndex(int index)
{
    m_combo->setCurrentIndex(index);
}
void RemoteFileSystemPathComboEdit::addItem(const QString &text)
{
    m_combo->addItem(text);
}
void RemoteFileSystemPathComboEdit::setEditText(const QString &text)
{
    m_combo->setEditText(text);
}
QString RemoteFileSystemPathComboEdit::currentText() const
{
    return m_combo->currentText();
}
