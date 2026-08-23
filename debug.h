#ifndef LWM_DEBUG_H_included
#define LWM_DEBUG_H_included

#include <map>
#include <string>
#include <vector>

#include "geometry.h"
#include "xlib.h"

class Client;

// Implements the built-in debug CLI. This is only available if -debugcli is
// passed on the command line.
class DebugCLI {
 public:
  DebugCLI();
  void Read();
  // Init runs commands on start-up (provided on the command line), and prints
  // out the hello message. Should be run even if there are no commands.
  void Init(const std::vector<std::string>& init_commands);

  static bool DebugEnabled(const Client* c);
  static bool DebugEnabled(Window w);
  static std::string NameFor(const Client* c);
  static std::string NameFor(Window w);

  // Called by LScr on client appearance/disappearance. Has no effect if
  // debugging is disabled.
  static void NotifyClientAdd(Client* c);
  static void NotifyClientRemove(Client* c);

  // Called by LScr::Furnish once a client's frame window exists, so the frame
  // can inherit the client's debug label. Has no effect if debugging is
  // disabled for that client.
  static void NotifyFrameCreated(Client* c);

 private:
  void ProcessLine(std::string line);
  void CmdXRandr(std::string line);
  void CmdDbg(std::string line);
  void ResetDeadZones(const std::vector<Rect>& visible);
  bool IsDebugEnabled(Window w);
  bool DisableDebugging(Window w);
  std::string LookupNameFor(Window w);

  bool debug_new_;

  // Windows which cover the areas of the desktop that are not visible, due to
  // the debug CLI fake xrandr commands.
  std::vector<Window> dead_zones_;

  // Windows we're debugging (value is their dbg name).
  std::map<Window, std::string> debug_windows_;
};

#endif  // LWM_DEBUG_H_included
