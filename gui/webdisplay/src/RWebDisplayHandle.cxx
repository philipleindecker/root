/// \file RWebDisplayHandle.cxx
/// \ingroup WebGui ROOT7
/// \author Sergey Linev <s.linev@gsi.de>
/// \date 2018-10-17
/// \warning This is part of the ROOT 7 prototype! It will change without notice. It might trigger earthquakes. Feedback
/// is welcome!

/*************************************************************************
 * Copyright (C) 1995-2018, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/RWebDisplayHandle.hxx>

#include <ROOT/RMakeUnique.hxx>
#include <ROOT/TLogger.hxx>

#include "ROOT/RWebWindow.hxx"
#include "ROOT/RWebWindowsManager.hxx"

#include "THttpServer.h"

#include "RConfigure.h"
#include "TSystem.h"
#include "TRandom.h"
#include "TString.h"
#include "TApplication.h"
#include "TTimer.h"
#include "TObjArray.h"
#include "TROOT.h"
#include "TEnv.h"

#if !defined(_MSC_VER)
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <spawn.h>
#else
#include <process.h>
#endif

std::map<std::string, std::unique_ptr<ROOT::Experimental::RWebDisplayHandle::Creator>> &ROOT::Experimental::RWebDisplayHandle::GetMap()
{
   static std::map<std::string, std::unique_ptr<ROOT::Experimental::RWebDisplayHandle::Creator>> sMap;
   return sMap;
}

std::unique_ptr<ROOT::Experimental::RWebDisplayHandle::Creator> &ROOT::Experimental::RWebDisplayHandle::FindCreator(const std::string &name, const std::string &libname)
{
   auto &m = GetMap();
   auto search = m.find(name);
   if (search == m.end()) {

      if (libname == "ChromeCreator") {
         m.emplace(name, std::make_unique<ChromeCreator>());
      } else if (libname == "FirefoxCreator") {
         m.emplace(name, std::make_unique<FirefoxCreator>());
      } else if (!libname.empty()) {
         gSystem->Load(libname.c_str());
      }

      search = m.find(name); // try again
   }

   if (search != m.end())
      return search->second;

   static std::unique_ptr<ROOT::Experimental::RWebDisplayHandle::Creator> dummy;
   return dummy;
}

namespace ROOT {
namespace Experimental {

class RWebBrowserHandle : public RWebDisplayHandle {

#if !defined(_MSC_VER)
   typedef pid_t browser_process_id;
#else
   typedef int browser_process_id;
#endif
   std::string fTmpDir;
   bool fHasPid{false};
   browser_process_id fPid;

public:
   RWebBrowserHandle(const std::string &url, const std::string &tmpdir) : RWebDisplayHandle(url), fTmpDir(tmpdir) {}

   RWebBrowserHandle(const std::string &url, const std::string &tmpdir, browser_process_id pid)
      : RWebDisplayHandle(url), fTmpDir(tmpdir), fHasPid(true), fPid(pid)
   {
   }

   virtual ~RWebBrowserHandle()
   {
#if !defined(_MSC_VER)
      if (fHasPid)
         kill(fPid, SIGKILL);
      if (!fTmpDir.empty())
         gSystem->Exec(TString::Format("rm -rf %s", fTmpDir.c_str()));
#else
      if (fHasPid)
         gSystem->Exec(TString::Format("taskkill /F /PID %d", fPid));
      if (!fTmpDir.empty())
         gSystem->Exec(TString::Format("rmdir /S /Q %s", fTmpDir.c_str()));
#endif
   }
};

} // namespace Experimental
} // namespace ROOT

//////////////////////////////////////////////////////////////////////////////////////////////////

ROOT::Experimental::RWebDisplayHandle::BrowserCreator::BrowserCreator(bool dflt, const std::string &where_arg)
{
   if (!dflt) return;

   if (!where_arg.empty()) {
      if (where_arg.find("$") != std::string::npos) {
         fExec = where_arg;
      } else {
         fExec = "$prog $url &";
         fProg = where_arg;
      }
   } else if (gSystem->InheritsFrom("TMacOSXSystem")) {
      fExec = "open \'$url\'";
   } else if (gSystem->InheritsFrom("TWinNTSystem")) {
      fExec = "start $url";
   } else {
      fExec = "xdg-open \'$url\' &";
   }
}

void ROOT::Experimental::RWebDisplayHandle::BrowserCreator::TestProg(const std::string &nexttry, bool check_std_paths)
{
   if (nexttry.empty() || !fProg.empty())
      return;

   if (!gSystem->AccessPathName(nexttry.c_str(), kExecutePermission)) {
      fProg = nexttry;
      #ifdef R__MACOSX
         fProg.ReplaceAll("%20"," ");
      #endif
      return;
   }

   if (!check_std_paths) return;

#ifdef _MSC_VER
   std::string ProgramFiles = gSystem->Getenv("ProgramFiles");
   size_t pos = ProgramFiles.find(" (x86)");
   if (pos != std::string::npos)
      ProgramFiles.erase(pos, 6);
   std::string ProgramFilesx86 = gSystem->Getenv("ProgramFiles(x86)");

   if (!ProgramFiles.empty())
      TestProg(ProgramFiles + nexttry, false);
   if (!ProgramFilesx86.empty())
      TestProg(ProgramFilesx86 + nexttry, false);
#endif
}


std::unique_ptr<ROOT::Experimental::RWebDisplayHandle>
ROOT::Experimental::RWebDisplayHandle::BrowserCreator::Make(THttpServer *, const std::string &url, bool batch, int width, int height)
{
   TString exec = batch ? fBatchExec.c_str() : fExec.c_str();

   if (exec.Length() == 0)
      return nullptr;

   std::string swidth = std::to_string(width > 0 ? width : 800);
   std::string sheight = std::to_string(height > 0 ? height : 600);

   std::string rmdir = MakeProfile(exec, batch);

   exec.ReplaceAll("$url", url.c_str());
   exec.ReplaceAll("$width", swidth.c_str());
   exec.ReplaceAll("$height", sheight.c_str());

   if (exec.Index("fork:") == 0) {
      if (fProg.empty()) {
         R__ERROR_HERE("WebDisplay") << "Fork instruction without executable";
         return nullptr;
      }

      exec.Remove(0, 5);
#if !defined(_MSC_VER)

      std::unique_ptr<TObjArray> args(exec.Tokenize(" "));
      if (!args || (args->GetLast()<=0)) {
         R__ERROR_HERE("WebDisplay") << "Fork instruction is empty";
         return nullptr;
      }

      std::vector<char *> argv;
      argv.push_back((char *) fProg.c_str());
      for (Int_t n = 0; n <= args->GetLast(); ++n)
         argv.push_back((char *)args->At(n)->GetName());
      argv.push_back(nullptr);

      R__DEBUG_HERE("WebDisplay") << "Show web window in browser with posix_spawn:\n" << fProg << " " << exec;

      pid_t pid;
      int status = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), nullptr);
      if (status != 0) {
         R__ERROR_HERE("WebDisplay") << "Fail to launch " << argv[0];
         return 0;
      }

      // add processid and rm dir

      return std::make_unique<RWebBrowserHandle>(url, rmdir, pid);

      // return win.AddProcId(batch_mode, key, std::string("pid:") + std::to_string((int)pid) + rmdir);

#else
      std::string tmp;
      char c;
      int pid;
      if (!fProg.empty()) {
         exec.Prepend(Form("wmic process call create \"%s", fProg.c_str()));
      } else {
         R__ERROR_HERE("WebDisplay") << "No Web browser found in Program Files!";
         return nullptr;
      }
      exec.Append("\" | find \"ProcessId\" ");
      TString process_id(gSystem->GetFromPipe(exec.Data()));
      std::stringstream ss(process_id.Data());
      ss >> tmp >> c >> pid;

      // add processid and rm dir
      return std::make_unique<RWebBrowserHandle>(url, rmdir, pid);

      //return win.AddProcId(batch_mode, key, std::string("pid:") + std::to_string((int)pid) + rmdir);
#endif
   }

   TString prog = fProg.c_str();

#ifdef R__MACOSX
   prog.ReplaceAll(" ", "\\ ");
#endif

#ifdef _MSC_VER
   std::unique_ptr<TObjArray> args(exec.Tokenize(" "));
   std::vector<char *> argv;
   if (prog.EndsWith("chrome.exe"))
      argv.push_back("chrome.exe");
   else if (prog.EndsWith("firefox.exe"))
      argv.push_back("firefox.exe");
   for (Int_t n = 1; n <= args->GetLast(); ++n)
      argv.push_back((char *)args->At(n)->GetName());
   argv.push_back(nullptr);
#endif

   exec.ReplaceAll("$prog", prog.Data());

   // unsigned connid = win.AddProcId(batch_mode, key, where + rmdir); // for now just application name

   R__DEBUG_HERE("WebDisplay") << "NEW Showing web window in browser with:\n" << exec;

#ifdef _MSC_VER
   _spawnv(_P_NOWAIT, prog.Data(), argv.data());
#else
   gSystem->Exec(exec);
#endif

   // add rmdir if required
   return std::make_unique<RWebBrowserHandle>(url, rmdir);
}

//////////////////////////////////////////////////////////////////////////////////////////////////


ROOT::Experimental::RWebDisplayHandle::ChromeCreator::ChromeCreator() : BrowserCreator(false)
{
   TestProg(gEnv->GetValue("WebGui.Chrome", ""));

#ifdef _MSC_VER
   TestProg("\\Google\\Chrome\\Application\\chrome.exe", true);
#endif
#ifdef R__MACOSX
   TestProg("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
#endif
#ifdef R__LINUX
   TestProg("/usr/bin/chromium");
   TestProg("/usr/bin/chromium-browser");
   TestProg("/usr/bin/chrome-browser");
#endif

#ifdef _MSC_VER
   fBatchExec = gEnv->GetValue("WebGui.ChromeBatch", "fork: --headless --disable-gpu $url");
   fExec = gEnv->GetValue("WebGui.ChromeInteractive", "$prog --window-size=$width,$height --app=$url");
#else
   fBatchExec = gEnv->GetValue("WebGui.ChromeBatch", "fork:--headless $url");
   fExec = gEnv->GetValue("WebGui.ChromeInteractive", "$prog --window-size=$width,$height --app=\'$url\' &");
#endif
}


//////////////////////////////////////////////////////////////////////////////////////////////////


ROOT::Experimental::RWebDisplayHandle::FirefoxCreator::FirefoxCreator() : BrowserCreator(false)
{
   TestProg(gEnv->GetValue("WebGui.Firefox", ""));

#ifdef _MSC_VER
   TestProg("\\Mozilla Firefox\\firefox.exe", true);
#endif
#ifdef R__MACOSX
   TestProg("/Applications/Firefox.app/Contents/MacOS/firefox");
#endif
#ifdef R__LINUX
   TestProg("/usr/bin/firefox");
#endif

#ifdef _MSC_VER
   // there is a problem when specifying the window size with wmic on windows:
   // It gives: Invalid format. Hint: <paramlist> = <param> [, <paramlist>].
   fBatchExec = gEnv->GetValue("WebGui.FirefoxBatch", "fork: -headless -no-remote $profile $url");
   fExec = gEnv->GetValue("WebGui.FirefoxInteractive", "$prog -width=$width -height=$height $profile $url");
#else
   fBatchExec = gEnv->GetValue("WebGui.FirefoxBatch", "fork:-headless -no-remote $profile $url");
   fExec = gEnv->GetValue("WebGui.FirefoxInteractive", "$prog -width $width -height $height $profile \'$url\' &");
#endif
}

std::string ROOT::Experimental::RWebDisplayHandle::FirefoxCreator::MakeProfile(TString &exec, bool batch_mode)
{
   std::string rmdir;

   if (exec.Index("$profile") == kNPOS)
      return rmdir;

   TString profile_arg;

   const char *ff_profile = gEnv->GetValue("WebGui.FirefoxProfile", "");
   const char *ff_profilepath = gEnv->GetValue("WebGui.FirefoxProfilePath", "");
   Int_t ff_randomprofile = gEnv->GetValue("WebGui.FirefoxRandomProfile", 0);
   if (ff_profile && *ff_profile) {
      profile_arg.Form("-P %s", ff_profile);
   } else if (ff_profilepath && *ff_profilepath) {
      profile_arg.Form("-profile %s", ff_profilepath);
   } else if ((ff_randomprofile > 0) || (batch_mode && (ff_randomprofile >= 0))) {

      gRandom->SetSeed(0);

      TString rnd_profile = TString::Format("root_ff_profile_%d", gRandom->Integer(0x100000));
      TString profile_dir = TString::Format("%s/%s", gSystem->TempDirectory(), rnd_profile.Data());

      profile_arg.Form("-profile %s", profile_dir.Data());
      if (!batch_mode)
         profile_arg.Prepend("-no-remote ");

      if (!fProg.empty()) {
         gSystem->Exec(Form("%s %s -no-remote -CreateProfile \"%s %s\"", fProg.c_str(), (batch_mode ? "-headless" : ""),
                            rnd_profile.Data(), profile_dir.Data()));

         rmdir = profile_dir.Data();
      } else {
         R__ERROR_HERE("WebDisplay") << "Cannot create Firefox profile without assigned executable, check WebGui.Firefox variable";
      }
   }

   exec.ReplaceAll("$profile", profile_arg.Data());

   return rmdir;
}



///////////////////////////////////////////////////////////////////////////////////////////////////
/// checks if provided executable exists

void ROOT::Experimental::RWebDisplayHandle::TestProg(TString &prog, const std::string &nexttry)
{
   if ((prog.Length()==0) && !nexttry.empty())
      if (!gSystem->AccessPathName(nexttry.c_str(), kExecutePermission))
          prog = nexttry.c_str();
}


unsigned ROOT::Experimental::RWebDisplayHandle::DisplayWindow(RWebWindow &win, bool batch_mode, const std::string &where)
{
   std::string key;
   std::string rmdir;
   int ntry = 100000;

   do {
      key = std::to_string(gRandom->Integer(0x100000));
   } while ((--ntry > 0) && win.HasKey(key));
   if (ntry == 0) {
      R__ERROR_HERE("WebDisplay") << "Fail to create unique key for the window";
      return 0;
   }

   std::string addr = win.fMgr->GetUrl(win, batch_mode, false);
   if (addr.find("?") != std::string::npos)
      addr.append("&key=");
   else
      addr.append("?key=");
   addr.append(key);

   enum { kCustom, kNative, kLocal, kChrome, kFirefox, kCEF, kQt5 } kind = kCustom;

   if (where == "local")
      kind = kLocal;
   else if (where.empty() || (where == "native"))
      kind = kNative;
   else if (where == "firefox")
      kind = kFirefox;
   else if ((where == "chrome") || (where == "chromium"))
      kind = kChrome;
   else if (where == "cef")
      kind = kCEF;
   else if (where == "qt5")
      kind = kQt5;
   else
      kind = kCustom; // all others kinds, normally name of alternative web browser

   if ((kind == kLocal) || (kind == kCEF)) {

      auto &creator = FindCreator("cef", "libROOTCefDisplay");

      if (creator) {
         auto handle = creator->Make(win.fMgr->GetServer(), addr, batch_mode, win.GetWidth(), win.GetHeight());
         if (!handle) {
            R__ERROR_HERE("WebDisplay") << "Cannot create CEF Web window";
            return 0;
         }

         return win.AddProcId(batch_mode, key, "cef");
      }

      if (kind == kCEF) {
         R__ERROR_HERE("WebDisplay") << "CEF libraries not found";
         return 0;
      }
   }

   if ((kind == kLocal) || (kind == kQt5)) {

      auto &creator = FindCreator("qt5", "libROOTQt5WebDisplay");

      if (creator) {
         auto handle = creator->Make(win.fMgr->GetServer(), addr, batch_mode, win.GetWidth(), win.GetHeight());
         if (!handle) {
            R__ERROR_HERE("WebDisplay") << "Cannot create Qt5 Web window";
            return 0;
         }

         return win.AddProcId(batch_mode, key, "qt5");
      }

      if (kind == kQt5) {
         R__ERROR_HERE("WebDisplay") << "Qt5 libraries not found";
         return 0;
      }
   }

   if ((kind == kLocal) || (kind == kQt5) || (kind == kCEF)) {
      R__ERROR_HERE("WebDisplay") << "Neither Qt5 nor CEF libraries were found to provide local display";
      return 0;
   }


   if ((kind == kNative) || (kind == kChrome)) {
      auto &creator = FindCreator("chrome", "ChromeCreator");

      if (creator) {

         if (!win.fMgr->CreateServer(true)) {
            R__ERROR_HERE("WebDisplay") << "Fail to start real HTTP server";
            return 0;
         }

         addr = win.fMgr->GetServerAddr() + addr;

         auto handle = creator->Make(win.fMgr->GetServer(), addr, batch_mode, win.GetWidth(), win.GetHeight());
         if (!handle) {
            R__ERROR_HERE("WebDisplay") << "Cannot create Chrome browser window";
            return 0;
         }

         return win.AddProcId(batch_mode, key, "chrome");
      }

      if (kind == kChrome) {
         R__ERROR_HERE("WebDisplay") << "Chrome browser cannot be started";
         return 0;
      }
   }


   if ((kind == kNative) || (kind == kFirefox)) {
      auto &creator = FindCreator("firefox", "FirefoxCreator");

      if (creator) {

         if (!win.fMgr->CreateServer(true)) {
            R__ERROR_HERE("WebDisplay") << "Fail to start real HTTP server";
            return 0;
         }

         addr = win.fMgr->GetServerAddr() + addr;

         auto handle = creator->Make(win.fMgr->GetServer(), addr, batch_mode, win.GetWidth(), win.GetHeight());
         if (!handle) {
            R__ERROR_HERE("WebDisplay") << "Cannot create Firefox browser window";
            return 0;
         }

         return win.AddProcId(batch_mode, key, "firefox");
      }

      if (kind == kFirefox) {
         R__ERROR_HERE("WebDisplay") << "Firefox browser cannot be started";
         return 0;
      }
   }


#ifdef _MSC_VER
   std::string ProgramFiles = gSystem->Getenv("ProgramFiles");
   size_t pos = ProgramFiles.find(" (x86)");
   if (pos != std::string::npos)
      ProgramFiles.erase(pos, 6);
   std::string ProgramFilesx86 = gSystem->Getenv("ProgramFiles(x86)");
#endif

   TString exec;

   std::string swidth = std::to_string(win.GetWidth() ? win.GetWidth() : 800);
   std::string sheight = std::to_string(win.GetHeight() ? win.GetHeight() : 600);
   TString prog;


   if ((kind == kNative) || (kind == kChrome)) {
      // see https://peter.sh/experiments/chromium-command-line-switches/

      TestProg(prog, gEnv->GetValue("WebGui.Chrome", ""));

#ifdef _MSC_VER
      std::string fullpath;
      if (!ProgramFiles.empty()){
         fullpath = ProgramFiles + "\\Google\\Chrome\\Application\\chrome.exe";
         TestProg(prog, fullpath);
      }
      if (!ProgramFilesx86.empty()) {
         fullpath = ProgramFilesx86 + "\\Google\\Chrome\\Application\\chrome.exe";
         TestProg(prog, fullpath);
      }
#endif
#ifdef R__MACOSX
      prog.ReplaceAll("%20"," ");
      TestProg(prog, "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
#endif
#ifdef R__LINUX
      TestProg(prog, "/usr/bin/chromium");
      TestProg(prog, "/usr/bin/chromium-browser");
      TestProg(prog, "/usr/bin/chrome-browser");
#endif
      if (prog.Length() > 0)
         kind = kChrome;
#ifdef _MSC_VER
      if (batch_mode)
         exec = gEnv->GetValue("WebGui.ChromeBatch", "fork: --headless --disable-gpu $url");
      else
         exec = gEnv->GetValue("WebGui.ChromeInteractive", "$prog --window-size=$width,$height --app=$url");
#else
      if (batch_mode)
         exec = gEnv->GetValue("WebGui.ChromeBatch", "fork:--headless $url");
      else
         exec = gEnv->GetValue("WebGui.ChromeInteractive", "$prog --window-size=$width,$height --app=\'$url\' &");
#endif
   }

   if ((kind == kFirefox) || ((kind == kNative) && (kind != kChrome))) {
      // to use firefox in batch mode at the same time as other firefox is running,
      // one should use extra profile. This profile should be created first:
      //    firefox -no-remote -CreateProfile root_batch
      // And then in the start command one should add:
      //    $prog -headless -no-remote -P root_batch -window-size=$width,$height $url
      // By default, no profile is specified, but this requires that no firefox is running

      TestProg(prog, gEnv->GetValue("WebGui.Firefox", ""));

#ifdef _MSC_VER
      std::string fullpath;
      if (!ProgramFiles.empty()) {
         fullpath = ProgramFiles + "\\Mozilla Firefox\\firefox.exe";
         TestProg(prog, fullpath);
      }
      if (!ProgramFilesx86.empty()) {
         fullpath = ProgramFilesx86 + "\\Mozilla Firefox\\firefox.exe";
         TestProg(prog, fullpath);
      }
#endif
#ifdef R__MACOSX
      prog.ReplaceAll("%20"," ");
      TestProg(prog, "/Applications/Firefox.app/Contents/MacOS/firefox");
#endif
#ifdef R__LINUX
      TestProg(prog, "/usr/bin/firefox");
#endif

      if (prog.Length() > 0)
         kind = kFirefox;

#ifdef _MSC_VER
      if (batch_mode)
         // there is a problem when specifying the window size with wmic on windows:
         // It gives: Invalid format. Hint: <paramlist> = <param> [, <paramlist>].
         exec = gEnv->GetValue("WebGui.FirefoxBatch", "fork: -headless -no-remote $profile $url");
      else
         exec = gEnv->GetValue("WebGui.FirefoxInteractive", "$prog -width=$width -height=$height $profile $url");
#else
      if (batch_mode)
         exec = gEnv->GetValue("WebGui.FirefoxBatch", "fork:-headless -no-remote $profile $url");
      else
         exec = gEnv->GetValue("WebGui.FirefoxInteractive", "$prog -width $width -height $height $profile \'$url\' &");
#endif

      if ((kind == kFirefox) && (exec.Index("$profile") != kNPOS)) {
         TString profile_arg;

         const char *ff_profile = gEnv->GetValue("WebGui.FirefoxProfile","");
         const char *ff_profilepath = gEnv->GetValue("WebGui.FirefoxProfilePath","");
         Int_t ff_randomprofile = gEnv->GetValue("WebGui.FirefoxRandomProfile", 0);
         if (ff_profile && *ff_profile) {
            profile_arg.Form("-P %s", ff_profile);
         } else if (ff_profilepath && *ff_profilepath) {
            profile_arg.Form("-profile %s", ff_profilepath);
         } else if ((ff_randomprofile > 0) || (batch_mode && (ff_randomprofile>=0))) {

            gRandom->SetSeed(0);

            TString rnd_profile = TString::Format("root_ff_profile_%d", gRandom->Integer(0x100000));
            TString profile_dir = TString::Format("%s/%s", gSystem->TempDirectory(), rnd_profile.Data());

            profile_arg.Form("-profile %s", profile_dir.Data());
            if (!batch_mode) profile_arg.Prepend("-no-remote ");

            gSystem->Exec(Form("%s %s -no-remote -CreateProfile \"%s %s\"", prog.Data(), (batch_mode ? "-headless" : ""), rnd_profile.Data(), profile_dir.Data()));

            rmdir = std::string("$rmdir$") + profile_dir.Data();
         }

         exec.ReplaceAll("$profile", profile_arg.Data());
      }
   }

   if ((kind != kFirefox) && (kind != kChrome)) {
      if (where == "native") {
         R__ERROR_HERE("WebDisplay") << "Neither firefox nor chrome are detected for native display";
         return 0;
      }

      if (batch_mode) {
         R__ERROR_HERE("WebDisplay") << "To use batch mode 'chrome' or 'firefox' should be configured as output";
         return 0;
      }

      if (kind != kNative) {
         if (where.find("$") != std::string::npos) {
            exec = where.c_str();
         } else {
            exec = "$prog $url &";
            prog = where.c_str();
         }
      } else if (gSystem->InheritsFrom("TMacOSXSystem")) {
         exec = "open \'$url\'";
      } else if (gSystem->InheritsFrom("TWinNTSystem")) {
         exec = "start $url";
      } else {
         exec = "xdg-open \'$url\' &";
      }
   }

   if (!win.fMgr->CreateServer(true)) {
      R__ERROR_HERE("WebDisplay") << "Fail to start real HTTP server";
      return 0;
   }

   addr = win.fMgr->GetServerAddr() + addr;

   exec.ReplaceAll("$url", addr.c_str());
   exec.ReplaceAll("$width", swidth.c_str());
   exec.ReplaceAll("$height", sheight.c_str());

   if (exec.Index("fork:") == 0) {
      exec.Remove(0, 5);
#if !defined(_MSC_VER)

      std::unique_ptr<TObjArray> args(exec.Tokenize(" "));
      if (!args || (args->GetLast()<=0)) {
         R__ERROR_HERE("WebDisplay") << "Fork instruction is empty";
         return 0;
      }

      std::vector<char *> argv;
      argv.push_back((char *) prog.Data());
      for (Int_t n = 0; n <= args->GetLast(); ++n)
         argv.push_back((char *)args->At(n)->GetName());
      argv.push_back(nullptr);

      R__DEBUG_HERE("WebDisplay") << "Show web window in browser with posix_spawn:\n" << prog << " " << exec;

      pid_t pid;
      int status = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), nullptr);
      if (status != 0) {
         R__ERROR_HERE("WebDisplay") << "Fail to launch " << argv[0];
         return 0;
      }

      return win.AddProcId(batch_mode, key, std::string("pid:") + std::to_string((int)pid) + rmdir);

#else
      std::string tmp;
      char c;
      int pid;
      if (prog.Length()) {
         exec.Prepend(Form("wmic process call create \"%s", prog.Data()));
      } else {
         R__ERROR_HERE("WebDisplay") << "No Web browser found in Program Files!";
         return 0;
      }
      exec.Append("\" | find \"ProcessId\" ");
      TString process_id(gSystem->GetFromPipe(exec.Data()));
      std::stringstream ss(process_id.Data());
      ss >> tmp >> c >> pid;
      return win.AddProcId(batch_mode, key, std::string("pid:") + std::to_string((int)pid) + rmdir);
#endif
   }

#ifdef R__MACOSX
   prog.ReplaceAll(" ", "\\ ");
#endif

#ifdef _MSC_VER
   std::unique_ptr<TObjArray> args(exec.Tokenize(" "));
   std::vector<char *> argv;
   if (prog.EndsWith("chrome.exe"))
      argv.push_back("chrome.exe");
   else if (prog.EndsWith("firefox.exe"))
      argv.push_back("firefox.exe");
   for (Int_t n = 1; n <= args->GetLast(); ++n)
      argv.push_back((char *)args->At(n)->GetName());
   argv.push_back(nullptr);
#endif

   exec.ReplaceAll("$prog", prog.Data());

   unsigned connid = win.AddProcId(batch_mode, key, where + rmdir); // for now just application name

   R__DEBUG_HERE("WebDisplay") << "Showing web window in browser with:\n" << exec;

#ifdef _MSC_VER
   _spawnv(_P_NOWAIT, prog.Data(), argv.data());
#else
   gSystem->Exec(exec);
#endif

   return connid;

}