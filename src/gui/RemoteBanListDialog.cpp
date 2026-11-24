// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteBanListDialog.h"
#include "ui_banlistoptionsdialog.h"

#include <QHostAddress>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStringListModel>
#include <algorithm>

namespace
{
QHostAddress strictAddress(const QString &text)
{
    const QHostAddress address(text);
    if (address.isNull() || !address.scopeId().isEmpty())
        return {};
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
    {
        // No ambiguous shortened, octal or hexadecimal IPv4 spellings.
        static const QRegularExpression dotted(
            QStringLiteral("^(0|[1-9][0-9]{0,2})(\\.(0|[1-9][0-9]{0,2})){3}$"));
        if (!dotted.match(text).hasMatch())
            return {};
    }
    return address;
}

bool ordered(const QHostAddress &first, const QHostAddress &last)
{
    if (first.isNull() || last.isNull() || first.protocol() != last.protocol())
        return false;
    if (first.protocol() == QAbstractSocket::IPv4Protocol)
        return first.toIPv4Address() <= last.toIPv4Address();
    if (first.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    const auto a = first.toIPv6Address();
    const auto b = last.toIPv6Address();
    return !std::lexicographical_compare(b.c, b.c + 16, a.c, a.c + 16);
}
}

QString RemoteBanListDialog::canonicalEntry(const QString &input, bool rangesSupported)
{
    const QString text = input.trimmed();
    const auto parts = text.split(QLatin1Char('-'));
    QHostAddress first, last;
    if (parts.size() == 2 && rangesSupported)
    {
        first = strictAddress(parts.at(0).trimmed());
        last = strictAddress(parts.at(1).trimmed());
    }
    else if (parts.size() == 1 && text.contains(QLatin1Char('/')) && rangesSupported)
    {
        const auto subnet = text.split(QLatin1Char('/'));
        if (subnet.size() != 2 || strictAddress(subnet.first()).isNull())
            return {};
        bool ok = false;
        const int prefix = subnet.last().toInt(&ok);
        if (!ok || prefix < 0 || QString::number(prefix) != subnet.last())
            return {};
        const auto parsed = QHostAddress::parseSubnet(text);
        first = parsed.first;
        if (first.isNull() || parsed.second != prefix)
            return {};
        if (first.protocol() == QAbstractSocket::IPv4Protocol && prefix <= 32)
        {
            const quint32 mask = prefix == 32 ? 0u : (0xffffffffu >> prefix);
            last = QHostAddress(first.toIPv4Address() | mask);
        }
        else if (first.protocol() == QAbstractSocket::IPv6Protocol && prefix <= 128)
        {
            auto bytes = first.toIPv6Address();
            for (int bit = prefix; bit < 128; ++bit)
                bytes.c[bit / 8] |= static_cast<quint8>(1u << (7 - bit % 8));
            last = QHostAddress(bytes);
        }
        else
            return {};
    }
    else if (parts.size() == 1 && !text.contains(QLatin1Char('/')))
        first = last = strictAddress(text);
    else
        return {};

    if (!ordered(first, last))
        return {};
    return first == last ? first.toString()
        : first.toString() + QStringLiteral(" - ") + last.toString();
}

bool RemoteBanListDialog::validList(const QString &entries, bool rangesSupported)
{
    // Upstream returns a sorted, canonical newline list. Refuse to replace
    // an unrepresentable server list rather than silently dropping entries.
    if (entries.isEmpty())
        return true;
    QStringList canonical;
    const auto lines = entries.split(QLatin1Char('\n'));
    for (const auto &line : lines)
    {
        const auto normalized = canonicalEntry(line, rangesSupported);
        if (normalized.isEmpty() || normalized != line)
            return false;
        canonical.append(normalized);
    }
    canonical.sort();
    canonical.removeDuplicates();
    return canonical == lines;
}

RemoteBanListDialog::RemoteBanListDialog(const QString &entries, bool rangesSupported, QWidget *parent)
    : QDialog(parent)
    , m_ui(std::make_unique<Ui::BanListOptionsDialog>())
    , m_model(new QStringListModel(entries.split(QLatin1Char('\n'), Qt::SkipEmptyParts), this))
    , m_rangesSupported(rangesSupported)
{
    m_ui->setupUi(this);
    m_ui->bannedIPList->setModel(m_model);
    m_ui->bannedIPList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ui->bannedIPList->sortByColumn(0, Qt::AscendingOrder);
    m_ui->buttonBanIP->setEnabled(false);
    m_ui->buttonDeleteIP->setEnabled(false);
    m_ui->txtIP->setToolTip(rangesSupported
        ? tr("Enter an IPv4 or IPv6 address, address range, or CIDR prefix.")
        : tr("This server supports individual IPv4 and IPv6 addresses only."));
    connect(m_ui->txtIP, &QLineEdit::textChanged, this, [this](const QString &value) {
        m_ui->buttonBanIP->setEnabled(!canonicalEntry(value, m_rangesSupported).isEmpty());
    });
    connect(m_ui->buttonBanIP, &QPushButton::clicked, this, [this]() {
        const QString entry = canonicalEntry(m_ui->txtIP->text(), m_rangesSupported);
        if (entry.isEmpty())
            return;
        QStringList list = m_model->stringList();
        if (list.contains(entry))
        {
            QMessageBox::warning(this, tr("Warning"), tr("The entered IP is already banned."));
            return;
        }
        list.append(entry);
        list.sort();
        m_model->setStringList(list);
        m_ui->txtIP->clear();
    });
    connect(m_ui->bannedIPList->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, [this]() {
            m_ui->buttonDeleteIP->setEnabled(m_ui->bannedIPList->selectionModel()->hasSelection());
        });
    connect(m_ui->buttonDeleteIP, &QPushButton::clicked, this, [this]() {
        auto indexes = m_ui->bannedIPList->selectionModel()->selectedIndexes();
        std::sort(indexes.begin(), indexes.end(), [](const QModelIndex &a, const QModelIndex &b) {
            return a.row() > b.row();
        });
        for (const auto &index : indexes)
            m_model->removeRow(index.row());
    });
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

RemoteBanListDialog::~RemoteBanListDialog() = default;

QString RemoteBanListDialog::entries() const
{
    auto list = m_model->stringList();
    list.sort();
    list.removeDuplicates();
    return list.join(QLatin1Char('\n'));
}
