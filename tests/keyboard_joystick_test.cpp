// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#include "KeyboardJoystick.h"
#include "KeyChord.h"
#include <cassert>

int main()
{
    using J = pom2::KeyboardJoystick;
    J j;
    assert(!j.key(263, J::Left, true, false, false)); // disabled: normal typing
    assert(j.paddle(0) == 128 && !j.button(0));
    j.setActive(true);
    assert(j.key(263, J::Left, true, false, false));
    assert(j.key(265, J::Up, true, false, false));
    assert(j.paddle(0) == 0 && j.paddle(1) == 0); // full diagonal
    assert(j.key(262, J::Right, true, false, false));
    assert(j.paddle(0) == 128); // opposing keys cancel
    assert(j.key(263, J::Left, false, false, false));
    assert(j.paddle(0) == 255); // other direction is still held
    assert(j.key(264, J::Down, true, false, false));
    assert(j.paddle(1) == 128);
    assert(j.key(265, J::Up, false, false, false));
    assert(j.paddle(1) == 255);

    // Desktop AZERTY: the physical W position is the letter Z.
    const char z = pom2::keychord::letterFromKeyName("z", 'W');
    assert(J::control('W', z) == J::Fire1);
    assert(j.key('W', J::control('W', z), true, false, false));
    assert(j.button(1) && j.suppressCharacter()); // no character callback leak
    assert(j.key('X', J::Fire0, true, false, false));
    assert(j.button(0) && j.button(1));
    // Releasing after a layout/modifier change still releases the right button.
    assert(j.key('W', J::None, false, false, true));
    assert(!j.button(1) && j.button(0));
    assert(!j.suppressCharacter());
    assert(j.key('X', J::Fire0, false, false, false));

    // Shortcut and unrelated character events do not inherit suppression.
    assert(!j.key('X', J::Fire0, true, false, true)); // Ctrl-X
    assert(!j.button(0) && !j.suppressCharacter());
    assert(!j.key('Z', J::Fire1, true, false, true)); // Alt/Super-Z
    assert(!j.key('A', J::None, true, false, false));
    assert(!j.suppressCharacter());

    // Focus loss / UI capture / disabling the setting all suspend input.
    assert(j.key('X', J::Fire0, true, false, false));
    j.setActive(false);
    assert(j.paddle(0) == 128 && j.paddle(1) == 128);
    assert(!j.button(0) && !j.button(1) && !j.suppressCharacter());
    assert(!j.key('X', J::Fire0, true, true, false)); // text field owns repeat
    j.setActive(true);
    assert(j.key('X', J::Fire0, true, true, false)); // swallow stale repeat
    assert(!j.button(0) && j.suppressCharacter()); // but never rearm fire
    j.key('X', J::Fire0, false, false, false);
    assert(j.key('X', J::Fire0, true, false, false));
    assert(j.button(0));
    j.setActive(true); // ordinary polls preserve held input
    assert(j.button(0));
    assert(!j.key('X', J::Fire0, true, true, true)); // Ctrl added while held
    assert(!j.button(0) && !j.suppressCharacter());

    // A repeat of an already held direction remains live.
    assert(j.key(263, J::Left, true, false, false));
    assert(j.key(263, J::Left, true, true, false));
    assert(j.paddle(0) == 0);
    assert(!j.key(-1, J::None, true, false, false));
    return 0;
}
