/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/wait.h>

#include <iostream>

#include <pthread.h>
#include <unistd.h>

#include <list>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>

#include <process/gtest.hpp>
#include <process/subprocess.hpp>

#include "linux/ns.hpp"

#include "tests/flags.hpp"
#include "tests/ns_tests.hpp"


using namespace mesos::internal;

using namespace process;

using std::list;
using std::set;
using std::string;
using std::vector;


// Helper for cloneChild() which expects an int(void*).
static int cloneChildHelper(void* _func)
{
  const lambda::function<int()>* func =
    static_cast<const lambda::function<int()>*> (_func);

  return (*func)();
}


static pid_t cloneChild(
    int flags,
    const lambda::function<int()>& func)

{
  // 8 MiB stack for child.
  static unsigned long long stack[(8*1024*1024)/sizeof(unsigned long long)];

  return ::clone(
      cloneChildHelper,
      &stack[sizeof(stack)/sizeof(stack[0]) - 1], // Stack grows down.
      flags | SIGCHLD,
      (void*) &func);
}


// Test that a child in different namespace(s) can setns back to the
// root namespace. We must fork a child to test this because setns
// doesn't support multi-threaded processes (which gtest is).
TEST(NsTest, ROOT_setns)
{
  // Clone then exec the setns-test-helper into a new namespace for
  // each available namespace.
  set<string> namespaces = ns::namespaces();
  ASSERT_FALSE(namespaces.empty());

  int flags = 0;

  foreach (const string& ns, namespaces) {
    Try<int> nstype = ns::nstype(ns);
    ASSERT_SOME(nstype);

    flags |= nstype.get();
  }

  vector<string> argv;
  argv.push_back("setns-test-helper");
  argv.push_back("test");

  Try<Subprocess> s = subprocess(
      path::join(tests::flags.build_dir, "src", "setns-test-helper"),
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      None(),
      None(),
      None(),
      lambda::bind(&cloneChild, flags, lambda::_1));

  // Continue in parent.
  ASSERT_SOME(s);

  // The child should exit 0.
  Future<Option<int>> status = s.get().status();
  AWAIT_READY(status);

  ASSERT_SOME(status.get());
  EXPECT_TRUE(WIFEXITED(status.get().get()));
  EXPECT_EQ(0, status.get().get());
}


static void* childThread(void* arg)
{
  // Newly created threads have PTHREAD_CANCEL_ENABLE and
  // PTHREAD_CANCEL_DEFERRED so they can be cancelled.
  while (true) { os::sleep(Seconds(1)); }

  return NULL;
}


// Test that setns correctly refuses to re-associate to a namespace if
// the caller is multi-threaded.
TEST(NsTest, ROOT_setnsMultipleThreads)
{
  set<string> namespaces = ns::namespaces();
  EXPECT_LT(0, namespaces.size());

  // Do not allow multi-threaded environment.
  pthread_t pthread;
  ASSERT_EQ(0, pthread_create(&pthread, NULL, childThread, NULL));

  foreach (const string& ns, namespaces) {
    EXPECT_ERROR(ns::setns(::getpid(), ns));
  }

  // Terminate the threads.
  EXPECT_EQ(0, pthread_cancel(pthread));
  EXPECT_EQ(0, pthread_join(pthread, NULL));
}
