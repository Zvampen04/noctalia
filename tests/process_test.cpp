#include "core/process/process.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "process_test: {}", message);
    }
    return condition;
  }

  std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += "'";
    return quoted;
  }

  bool capturedAsyncDeliversCallbacksAndResult() {
    std::mutex mutex;
    std::condition_variable cv;
    std::string stdOut;
    std::string stdErr;
    std::optional<process::RunResult> result;
    bool completed = false;

    process::RunCallbacks callbacks;
    callbacks.stdOut = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      stdOut.append(chunk);
    };
    callbacks.stdErr = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      stdErr.append(chunk);
    };
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      completed = true;
      cv.notify_one();
    };

    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 3;

    const bool launched =
        process::runAsync({"/bin/sh", "-lc", "printf abcdef; printf XYZ >&2"}, std::move(callbacks), options);
    if (!expect(launched, "captured async command did not launch")) {
      return false;
    }

    std::unique_lock lock(mutex);
    bool ok = expect(
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }), "captured async command did not complete"
    );
    ok = expect(result.has_value(), "captured async command did not provide a result") && ok;
    ok = expect(stdOut == "abcdef", "stdout callback did not receive full output") && ok;
    ok = expect(stdErr == "XYZ", "stderr callback did not receive full output") && ok;
    if (result.has_value()) {
      ok = expect(result->exitCode == 0, "captured async exit code was not zero") && ok;
      ok = expect(result->out == "abc", "captured async stdout result did not respect output limit") && ok;
      ok = expect(result->err == "XYZ", "captured async stderr result was wrong") && ok;
      ok = expect(result->outTruncated, "captured async stdout result was not marked truncated") && ok;
      ok = expect(!result->errTruncated, "captured async stderr result was incorrectly marked truncated") && ok;
      ok = expect(!result->timedOut, "captured async command timed out unexpectedly") && ok;
    }
    return ok;
  }

  bool capturedAsyncDeliversCompletionOnly() {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<process::RunResult> result;
    bool completed = false;

    process::RunCallbacks callbacks;
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      completed = true;
      cv.notify_one();
    };

    const bool launched = process::runAsync({"/bin/sh", "-c", "printf ok; exit 7"}, std::move(callbacks));
    if (!expect(launched, "completion-only async command did not launch")) {
      return false;
    }

    std::unique_lock lock(mutex);
    bool ok = expect(
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }),
        "completion-only async command did not complete"
    );
    ok = expect(result.has_value(), "completion-only async command did not provide a result") && ok;
    if (result.has_value()) {
      ok = expect(result->exitCode == 7, "completion-only async command exit code was wrong") && ok;
      ok = expect(result->out == "ok", "completion-only async command stdout was wrong") && ok;
      ok = expect(result->err.empty(), "completion-only async command stderr was not empty") && ok;
    }
    return ok;
  }

  bool syncAppliesEnvOverrides() {
    ::setenv("NOCTALIA_PROCESS_UNSET_TEST", "parent", 1);

    process::RunOptions options;
    options.env.push_back({"NOCTALIA_PROCESS_SET_TEST", "child"});
    options.env.push_back({"NOCTALIA_PROCESS_UNSET_TEST", std::nullopt});

    const auto result = process::runSync(
        {"/bin/sh", "-lc", R"(printf '%s/%s' "$NOCTALIA_PROCESS_SET_TEST" "${NOCTALIA_PROCESS_UNSET_TEST-unset}")"},
        options
    );
    ::unsetenv("NOCTALIA_PROCESS_UNSET_TEST");

    bool ok = expect(result.exitCode == 0, "sync env override command failed");
    ok = expect(result.out == "child/unset", "sync env overrides were not visible in child") && ok;
    return ok;
  }

  bool stringCommandsSupportShellComposition() {
    const auto result = process::runSync("printf first && printf second");
    bool ok = expect(result.exitCode == 0, "composed shell command failed");
    ok = expect(result.out == "firstsecond", "composed shell command did not execute both commands") && ok;
    return ok;
  }

  bool detachedAsyncInheritsLaunchEnvironment() {
    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() / ("noctalia_process_env_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    ::setenv("NOCTALIA_WALLPAPER_PATH", "/tmp/noctalia test/wallpaper.png", 1);
    ::setenv("NOCTALIA_WALLPAPER_CONNECTOR", "DP-1", 1);

    const std::string command = R"(printf '%s\n%s' "$NOCTALIA_WALLPAPER_PATH" "$NOCTALIA_WALLPAPER_CONNECTOR" > )"
        + shellQuote(outPath.string());
    const bool launched = process::runAsync(command);
    ::unsetenv("NOCTALIA_WALLPAPER_PATH");
    ::unsetenv("NOCTALIA_WALLPAPER_CONNECTOR");

    if (!expect(launched, "detached async env command did not launch")) {
      return false;
    }

    std::string contents;
    for (int i = 0; i < 50; ++i) {
      std::ifstream in(outPath);
      contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (contents == "/tmp/noctalia test/wallpaper.png\nDP-1") {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::filesystem::remove(outPath, ec);
    return expect(contents == "/tmp/noctalia test/wallpaper.png\nDP-1", "detached async env was not visible in child");
  }

  bool commandExistsRejectsDirectories() {
    bool ok = true;
    ok =
        expect(!process::commandExists("/usr/bin"), "commandExists should return false for /usr/bin (directory)") && ok;
    ok = expect(!process::commandExists("/"), "commandExists should return false for / (directory)") && ok;
    ok = expect(process::commandExists("true"), "commandExists should return true for 'true' on PATH") && ok;
    ok = expect(!process::commandExists(""), "commandExists should return false for empty string") && ok;
    ok =
        expect(!process::commandExists("/nonexistent"), "commandExists should return false for nonexistent path") && ok;
    return ok;
  }

  bool cgroupDetectsSystemdUserManager() {
    bool ok = true;
    ok = expect(
             process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@niri.service\n", 1000
             ),
             "uwsm compositor unit should be detected as user-manager managed"
         )
        && ok;
    ok = expect(
             process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1000.slice/user@1000.service/app.slice/noctalia.service\n", 1000
             ),
             "noctalia user service should be detected as user-manager managed"
         )
        && ok;
    ok = expect(
             !process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1000.slice/session-2.scope/noctalia\n", 1000
             ),
             "login session scope should not be detected as user-manager managed"
         )
        && ok;
    ok = expect(
             !process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1001.slice/user@1001.service/app.slice/noctalia.service\n", 1000
             ),
             "another user's manager should not be detected as ours"
         )
        && ok;
    ok = expect(
             process::cgroupIndicatesSystemdUserManager(
                 "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/noctalia.service\n", 1000
             ),
             "legacy cgroup v1 dump should be detected as user-manager managed"
         )
        && ok;
    ok = expect(!process::cgroupIndicatesSystemdUserManager("", 1000), "empty cgroup dump should not be managed") && ok;
    return ok;
  }

  bool detachedMissingBinaryReturnsFalse() {
    return expect(
        !process::runAsync(std::vector<std::string>{"/nonexistent/noctalia-missing-binary-xyz"}),
        "detached launch of a missing binary should return false"
    );
  }

  bool applicationScopePreservesArgvAndOptIn() {
    const std::vector<std::string> args = {
        "browser", "--unit=literal", "two words; $(literal)", "line\nbreak", "$1/$PWD/${NOCTALIA_LITERAL}/$$",
    };
    bool ok = expect(
        process::prepareApplicationCommand(args, false, true) == args,
        "disabled application isolation must preserve the original command"
    );
    ok = expect(
             process::prepareApplicationCommand(args, true, false) == args,
             "a shell outside the user manager must preserve its original launch path"
         )
        && ok;
    auto expected = std::vector<std::string>{
        "choom",
        "-n",
        "200",
        "--",
        "systemd-run",
        "--user",
        "--scope",
        "--collect",
        "--quiet",
        "--slice=app.slice",
        "--expand-environment=no",
        "--",
    };
    expected.insert(expected.end(), args.begin(), args.end());
    ok = expect(
             process::prepareApplicationCommand(args, true, true) == expected,
             "app scope must reset inherited protection and retain exact argument boundaries"
         )
        && ok;
    ok = expect(process::prepareApplicationCommand({}, true, true).empty(), "empty argv must stay invalid") && ok;
    process::setSystemdApplicationScopesEnabled(true);
    ok = expect(
             process::prepareApplicationCommand(args)
                 == process::prepareApplicationCommand(args, true, process::runningUnderSystemdUserManager()),
             "effective application policy must also require a managed user session"
         )
        && ok;
    process::setSystemdApplicationScopesEnabled(false);
    return ok;
  }

  bool scopedAsyncPreservesOutputAndExit() {
    char directory[] = "/tmp/noctalia-app-scope-XXXXXX";
    if (mkdtemp(directory) == nullptr) {
      return expect(false, "could not create app scope fixture");
    }
    const std::filesystem::path fixture(directory);
    const auto writeFixture = [&](const char* name, const char* script) {
      const auto path = fixture / name;
      std::ofstream(path) << script;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    };
    // Broker fixtures validate the external CLI contract and then exec the
    // untouched child. They need no real user bus or memory pressure.
    writeFixture("choom", R"(#!/bin/sh
test "$1" = -n && test "$2" = 200 && test "$3" = -- || exit 91
shift 3
exec "$@"
)");
    writeFixture("systemd-run", R"(#!/bin/sh
for expected in --user --scope --collect --quiet --slice=app.slice --expand-environment=no --; do
  test "$1" = "$expected" || exit 92
  shift
done
exec "$@"
)");
    const char* oldPathRaw = std::getenv("PATH");
    const bool hadOldPath = oldPathRaw != nullptr;
    const std::string oldPath = hadOldPath ? oldPathRaw : "";
    const std::string fixturePath = fixture.string() + ":" + oldPath;
    const std::string payload = "--unit=literal; $(not executed)\n$1/$PWD/${NOCTALIA_LITERAL}/$$";
    const auto command = process::prepareApplicationCommand(
        {"/bin/sh", "-c", "printf '%s' \"$1\"; printf error >&2; exit 7", "--", payload}, true, true
    );
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<process::RunResult> result;
    std::string output;
    std::string error;
    process::RunCallbacks callbacks;
    callbacks.stdOut = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      output.append(chunk);
    };
    callbacks.stdErr = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      error.append(chunk);
    };
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      ready.notify_one();
    };
    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 5;
    options.env.push_back({"PATH", fixturePath});
    bool ok = expect(process::runAsync(command, std::move(callbacks), options), "scoped async command was rejected");
    {
      std::unique_lock lock(mutex);
      ok = expect(
               ready.wait_for(lock, std::chrono::seconds(5), [&] { return result.has_value(); }),
               "scoped async result callback did not run"
           )
          && ok;
      ok = expect(output == payload && error == "error", "scope wrapper changed stdout/stderr or argv") && ok;
      if (result) {
        ok = expect(result->exitCode == 7 && !result->timedOut, "scope wrapper changed the child exit status") && ok;
        ok = expect(result->out == "--uni" && result->outTruncated, "scope wrapper lost the output limit") && ok;
        ok = expect(result->err == "error" && !result->errTruncated, "scope wrapper changed stderr capture") && ok;
      }
    }

    const auto detachedOutput = fixture / "launch-environment";
    ::setenv("PATH", fixturePath.c_str(), 1);
    const auto syncResult = process::runSyncWithTimeoutAndOutputLimit(command, std::chrono::seconds(2), 5);
    ok = expect(
             syncResult.exitCode == 7
                 && syncResult.out == "--uni"
                 && syncResult.outTruncated
                 && syncResult.err == "error"
                 && !syncResult.timedOut,
             "scoped synchronous helper lost its exit status or output limit"
         )
        && ok;
    const auto timeoutCommand =
        process::prepareApplicationCommand({"/bin/sh", "-c", "printf abcdefgh; exec sleep 10"}, true, true);
    const auto timeoutResult = process::runSyncWithTimeoutAndOutputLimit(timeoutCommand, std::chrono::seconds(1), 5);
    ok = expect(
             timeoutResult.timedOut && timeoutResult.out == "abcde" && timeoutResult.outTruncated,
             "scoped synchronous helper lost its timeout or output limit"
         )
        && ok;
    const auto detachedCommand = process::prepareApplicationCommand(
        {"/bin/sh", "-c",
         "printf '%s/%s/%s' \"$XDG_ACTIVATION_TOKEN\" \"$DESKTOP_STARTUP_ID\" \"$PWD\" > launch-environment"},
        true, true
    );
    ok = expect(
             process::runAsync(detachedCommand, "launch token", fixture.string()), "scoped detached launch was rejected"
         )
        && ok;
    if (hadOldPath) {
      ::setenv("PATH", oldPath.c_str(), 1);
    } else {
      ::unsetenv("PATH");
    }
    std::string environment;
    const std::string expectedEnvironment = "launch token/launch token/" + fixture.string();
    for (int attempt = 0; attempt < 100; ++attempt) {
      std::ifstream input(detachedOutput);
      environment.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
      if (environment == expectedEnvironment) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ok = expect(environment == expectedEnvironment, "scope wrapper lost activation tokens or working directory") && ok;
    std::filesystem::remove_all(fixture);
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = expect(!process::runAsync("true", process::RunCallbacks{}), "empty callback set should not launch") && ok;
  ok = capturedAsyncDeliversCallbacksAndResult() && ok;
  ok = capturedAsyncDeliversCompletionOnly() && ok;
  ok = syncAppliesEnvOverrides() && ok;
  ok = stringCommandsSupportShellComposition() && ok;
  ok = detachedAsyncInheritsLaunchEnvironment() && ok;
  ok = detachedMissingBinaryReturnsFalse() && ok;
  ok = commandExistsRejectsDirectories() && ok;
  ok = cgroupDetectsSystemdUserManager() && ok;
  ok = applicationScopePreservesArgvAndOptIn() && ok;
  ok = scopedAsyncPreservesOutputAndExit() && ok;
  return ok ? 0 : 1;
}
