/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000T_CONFIGURATION_DIALOG_H
#define P2000T_CONFIGURATION_DIALOG_H

#include <array>

#include <QDialog>

#include "pico_configuration.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

class ConfigurationDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit ConfigurationDialog(const PicoConfiguration &configuration,
                                 QWidget *parent = nullptr);

    PicoConfiguration configuration() const;
    void setConfiguration(const PicoConfiguration &configuration);

  signals:
    void applyRequested(const PicoConfiguration &configuration);
    void saveRequested(const PicoConfiguration &configuration);
    void reloadRequested();
    void defaultsRequested();

  private:
    void chooseColor(int index);
    void updateColorButton(int index);
    void updateStorageStatus(const PicoConfiguration &configuration);

    QSpinBox *vertical_ = nullptr;
    QSpinBox *phase_ = nullptr;
    QSpinBox *horizontal_ = nullptr;
    QComboBox *artwork_ = nullptr;
    QLabel *storage_status_ = nullptr;
    QPushButton *reload_ = nullptr;
    std::array<QPushButton *, P2000T_CONTROL_PALETTE_COLORS> color_buttons_{};
    std::array<QSpinBox *, P2000T_CONTROL_PALETTE_COLORS> red_{};
    std::array<QSpinBox *, P2000T_CONTROL_PALETTE_COLORS> green_{};
    std::array<QSpinBox *, P2000T_CONTROL_PALETTE_COLORS> blue_{};
};

#endif
