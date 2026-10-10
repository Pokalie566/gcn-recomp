// The system's own dialog for choosing a file: a disc image, for the launcher's "Browse".
// Modal: it returns once the player has chosen (the path) or cancelled (empty).
#pragma once
#include <string>

std::string ui_pick_disc(const std::string& title);
