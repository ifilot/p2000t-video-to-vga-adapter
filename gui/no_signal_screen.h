/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_GUI_NO_SIGNAL_SCREEN_H
#define P2000T_GUI_NO_SIGNAL_SCREEN_H

#include <QImage>
#include <QString>
#include <QtGlobal>

/** Return the user-facing name of one embedded VGA artwork selection. */
QString p2000tArtworkName(quint8 artwork);

/** Reproduce the selected firmware no-connection screen pixel for pixel. */
QImage renderP2000tNoSignalScreen(quint8 artwork);

#endif
