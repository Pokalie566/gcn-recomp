// The log and the crash message.
//
// Everything written to stderr -- the runtime's, Aurora's, SDL's -- also goes to a log file
// beside the player's state, <game>.log; the run before keeps its own as <game>.old.log, so
// a crash's log survives the run that comes next to look at it.
//
// A crash (a fault, a fatal error, a panic of the game's) says what happened and where the
// log is, in the log and to the player: a dialog, shown by a second process of the same
// executable (--crash-report), since the one that crashed may not be able to draw anything.
#pragma once
#include <string>

// From now on stderr goes to `path` too; a file already there is kept as <path>.old.log.
void log_start(const std::string& path);

// The crash, `what` in a sentence: in the log, then the dialog. GCN_CRASH_DIALOG=print
// prints the dialog's text instead of showing it (for a scripted check), =0 shows nothing.
void crash_report(const char* what);
// Its sentences in the player's language: ui_trf, once the menus know it.
void crash_set_translator(std::string (*trf)(const std::string& english, const std::string& arg));

// The second process's part: if the arguments ask for the dialog, it is shown and true
// returned (the process then exits).
bool crash_report_main(int argc, char** argv);
