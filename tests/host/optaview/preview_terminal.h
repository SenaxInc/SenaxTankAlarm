// The PC preview's terminal (optaview_preview.cpp), which stands for the Opta's USB port: lines,
// one at a time, from a file descriptor that is read without waiting. optaview_test.cpp tests it
// on a pipe.
//
// It reads only when no whole line is waiting, and then one chunk at most, so that a terminal
// that never stops sending (a long paste, or a program at the other end) cannot keep the web
// page from its turn, and what waits here stays small. Of a line it keeps OV_LINE_MAX characters
// at most, one more than a console line can have, so a line that was too long is still too long
// when it is handed on. The Opta's loop() takes the USB port the same way (OvLineEditor::take).

#ifndef OPTAVIEW_PREVIEW_TERMINAL_H
#define OPTAVIEW_PREVIEW_TERMINAL_H

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <unistd.h>

#include <deque>
#include <string>

#include "OptaViewConsole.h"

namespace optaview {

class PreviewTerminal : public OvConsoleHost {
 public:
  static const size_t CHUNK = 256;         // what one read takes, at most
  static const int DISCARD_CHUNKS = 16;    // what a key press that stops a command may take along

  explicit PreviewTerminal(int fd) : fd_(fd), open_(true) {}

  // A line that has been typed, without its line ending. False when there is none yet.
  bool takeLine(std::string *line) {
    if (lines_.empty()) {
      fill();
    }
    if (lines_.empty()) {
      return false;
    }
    line->swap(lines_.front());
    lines_.pop_front();
    return true;
  }

  // On a terminal a "key press" is a line: the terminal hands over what was typed at Enter. It
  // reads only when nothing at all is waiting here.
  bool inputPending() override {
    if (lines_.empty() && partial_.empty()) {
      fill();
    }
    return !lines_.empty() || !partial_.empty();
  }

  // The key press that stopped a command, with what came along with it.
  void discardInput() override {
    for (int i = 0; i < DISCARD_CHUNKS && fill(); ++i) {
    }
    lines_.clear();
    partial_.clear();
  }

  // False once the terminal has gone: the page is still served.
  bool open() const { return open_; }

  // Whole lines that wait here, read but not yet taken.
  size_t waiting() const { return lines_.size(); }

 private:
  int fd_;
  bool open_;
  std::deque<std::string> lines_;
  std::string partial_;  // the line that is still being typed

  // Reads one chunk if one has arrived. True when it read something.
  bool fill() {
    if (!open_) {
      return false;
    }
    struct pollfd ready = {fd_, POLLIN, 0};
    if (::poll(&ready, 1, 0) <= 0) {
      return false;
    }
    char chunk[CHUNK];
    const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
    if (got <= 0) {
      if (!(got < 0 && errno == EINTR)) {
        open_ = false;  // no terminal (any more)
      }
      return false;
    }
    for (ssize_t i = 0; i < got; ++i) {
      if (chunk[i] == '\n') {
        if (!partial_.empty() && partial_[partial_.size() - 1] == '\r') {
          partial_.erase(partial_.size() - 1);
        }
        lines_.push_back(std::string());
        lines_.back().swap(partial_);
      } else if (partial_.size() < OV_LINE_MAX) {
        partial_ += chunk[i];
      }
    }
    return true;
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_PREVIEW_TERMINAL_H
