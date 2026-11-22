// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QScrollArea>

// Static empty-state filter sections mirroring the upstream qBittorrent GUI.
// Future remote models will populate counters and apply selections.
class FilterSidebar final : public QScrollArea
{
public:
    explicit FilterSidebar(QWidget *parent = nullptr);
};
