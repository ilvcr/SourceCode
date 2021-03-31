/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/init/Init.h>

#include <glog/logging.h>

#include <folly/Singleton.h>
#include <folly/init/Phase.h>
#include <folly/logging/Init.h>
#include <folly/portability/Config.h>
#include <folly/synchronization/HazptrThreadPoolExecutor.h>

#if FOLLY_USE_SYMBOLIZER
#include <folly/experimental/symbolizer/SignalHandler.h> // @manual
#endif
#include <folly/portability/GFlags.h>

DEFINE_string(logging, "", "Logging configuration");

namespace folly {

void init(int* argc, char*** argv, bool removeFlags) {
#if FOLLY_USE_SYMBOLIZER
  // Install the handler now, to trap errors received during startup.
  // The callbacks, if any, can be installed later
  folly::symbolizer::installFatalSignalHandler();
#elif !defined(_WIN32)
  google::InstallFailureSignalHandler();
#endif

  // Indicate ProcessPhase::Regular and register handler to
  // indicate ProcessPhase::Exit.
  folly::set_process_phases();

  // Move from the registration phase to the "you can actually instantiate
  // things now" phase.
  folly::SingletonVault::singleton()->registrationComplete();

#if !FOLLY_HAVE_LIBGFLAGS
  (void)removeFlags;
#else
  gflags::ParseCommandLineFlags(argc, argv, removeFlags);
#endif

  folly::initLoggingOrDie(FLAGS_logging);
  auto programName = argc && argv && *argc > 0 ? (*argv)[0] : "unknown";
  google::InitGoogleLogging(programName);

#if FOLLY_USE_SYMBOLIZER
  // Don't use glog's DumpStackTraceAndExit; rely on our signal handler.
  google::InstallFailureFunction(abort);

  // Actually install the callbacks into the handler.
  folly::symbolizer::installFatalSignalCallbacks();
#endif
  // Set the default hazard pointer domain to use a thread pool executor
  // for asynchronous reclamation
  folly::enable_hazptr_thread_pool_executor();
}

Init::Init(int* argc, char*** argv, bool removeFlags) {
  init(argc, argv, removeFlags);
}

Init::~Init() {
  SingletonVault::singleton()->destroyInstances();
}
} // namespace folly