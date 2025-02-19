/*
 * Copyright (c) 2020, the SerenityOS developers.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "CellType/Type.h"
#include "Forward.h"
#include <LibGUI/Dialog.h>

namespace Spreadsheet {

class CellTypeDialog : public GUI::Dialog {
    C_OBJECT(CellTypeDialog);

public:
    CellTypeMetadata metadata() const;
    const CellType* type() const { return m_type; }

    enum class HorizontalAlignment : int {
        Left = 0,
        Center,
        Right,
    };
    enum class VerticalAlignment : int {
        Top = 0,
        Center,
        Bottom,
    };

private:
    CellTypeDialog(const Vector<Position>&, Sheet&, GUI::Window* parent = nullptr);
    void setup_tabs(GUI::TabWidget&, const Vector<Position>&, Sheet&);

    const CellType* m_type { nullptr };

    int m_length { -1 };
    String m_format;
    HorizontalAlignment m_horizontal_alignment { HorizontalAlignment::Right };
    VerticalAlignment m_vertical_alignment { VerticalAlignment::Center };
    Optional<Color> m_static_foreground_color;
    Optional<Color> m_static_background_color;
};

}
