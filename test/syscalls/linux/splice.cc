// Copyright 2019 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "test/util/file_descriptor.h"
#include "test/util/temp_path.h"
#include "test/util/test_util.h"
#include "test/util/thread_util.h"

namespace gvisor {
namespace testing {

namespace {

TEST(SpliceTest, TwoRegularFiles) {
  // Create temp files.
  const TempPath in_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());
  const TempPath out_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());

  // Open the input file as read only.
  const FileDescriptor inf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(in_file.path(), O_RDONLY));

  // Open the output file as write only.
  const FileDescriptor outf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(out_file.path(), O_WRONLY));

  // Verify that it is rejected as expected; regardless of offsets.
  loff_t in_offset = 0;
  loff_t out_offset = 0;
  EXPECT_THAT(splice(inf.get(), &in_offset, outf.get(), &out_offset, 1, 0),
              SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(splice(inf.get(), nullptr, outf.get(), &out_offset, 1, 0),
              SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(splice(inf.get(), &in_offset, outf.get(), nullptr, 1, 0),
              SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(splice(inf.get(), nullptr, outf.get(), nullptr, 1, 0),
              SyscallFailsWithErrno(EINVAL));
}

TEST(SpliceTest, SamePipe) {
  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Fill the pipe.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(wfd.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Attempt to splice to itself.
  EXPECT_THAT(splice(rfd.get(), nullptr, wfd.get(), nullptr, kPageSize, 0),
              SyscallFailsWithErrno(EINVAL));
}

TEST(TeeTest, SamePipe) {
  SKIP_IF(IsRunningOnGvisor());

  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Fill the pipe.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(wfd.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Attempt to tee to itself.
  EXPECT_THAT(tee(rfd.get(), wfd.get(), kPageSize, 0),
              SyscallFailsWithErrno(EINVAL));
}

TEST(TeeTest, RegularFile) {
  SKIP_IF(IsRunningOnGvisor());

  // Open some file.
  const TempPath in_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());
  const FileDescriptor inf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(in_file.path(), O_RDWR));

  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Attempt to tee from the file.
  EXPECT_THAT(tee(inf.get(), wfd.get(), kPageSize, 0),
              SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(tee(rfd.get(), inf.get(), kPageSize, 0),
              SyscallFailsWithErrno(EINVAL));
}

TEST(SpliceTest, PipeOffsets) {
  // Create two new pipes.
  int first[2], second[2];
  ASSERT_THAT(pipe(first), SyscallSucceeds());
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  ASSERT_THAT(pipe(second), SyscallSucceeds());
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // All pipe offsets should be rejected.
  loff_t in_offset = 0;
  loff_t out_offset = 0;
  EXPECT_THAT(splice(rfd1.get(), &in_offset, wfd2.get(), &out_offset, 1, 0),
              SyscallFailsWithErrno(ESPIPE));
  EXPECT_THAT(splice(rfd1.get(), nullptr, wfd2.get(), &out_offset, 1, 0),
              SyscallFailsWithErrno(ESPIPE));
  EXPECT_THAT(splice(rfd1.get(), &in_offset, wfd2.get(), nullptr, 1, 0),
              SyscallFailsWithErrno(ESPIPE));
}

// Event FDs may be used with splice without an offset.
TEST(SpliceTest, FromEventFD) {
  // Open the input eventfd with an initial value so that it is readable.
  constexpr uint64_t kEventFDValue = 1;
  int efd;
  ASSERT_THAT(efd = eventfd(kEventFDValue, 0), SyscallSucceeds());
  const FileDescriptor inf(efd);

  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Splice 8-byte eventfd value to pipe.
  constexpr int kEventFDSize = 8;
  EXPECT_THAT(splice(inf.get(), nullptr, wfd.get(), nullptr, kEventFDSize, 0),
              SyscallSucceedsWithValue(kEventFDSize));

  // Contents should be equal.
  std::vector<char> rbuf(kEventFDSize);
  ASSERT_THAT(read(rfd.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kEventFDSize));
  EXPECT_EQ(memcmp(rbuf.data(), &kEventFDValue, rbuf.size()), 0);
}

// Event FDs may not be used with splice with an offset.
TEST(SpliceTest, FromEventFDOffset) {
  int efd;
  ASSERT_THAT(efd = eventfd(0, 0), SyscallSucceeds());
  const FileDescriptor inf(efd);

  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Attempt to splice 8-byte eventfd value to pipe with offset.
  //
  // This is not allowed because eventfd doesn't support pread.
  constexpr int kEventFDSize = 8;
  loff_t in_off = 0;
  EXPECT_THAT(splice(inf.get(), &in_off, wfd.get(), nullptr, kEventFDSize, 0),
              SyscallFailsWithErrno(EINVAL));
}

// Event FDs may not be used with splice with an offset.
TEST(SpliceTest, ToEventFDOffset) {
  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Fill with a value.
  constexpr int kEventFDSize = 8;
  std::vector<char> buf(kEventFDSize);
  buf[0] = 1;
  ASSERT_THAT(write(wfd.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kEventFDSize));

  int efd;
  ASSERT_THAT(efd = eventfd(0, 0), SyscallSucceeds());
  const FileDescriptor outf(efd);

  // Attempt to splice 8-byte eventfd value to pipe with offset.
  //
  // This is not allowed because eventfd doesn't support pwrite.
  loff_t out_off = 0;
  EXPECT_THAT(splice(rfd.get(), nullptr, outf.get(), &out_off, kEventFDSize, 0),
              SyscallFailsWithErrno(EINVAL));
}

TEST(SpliceTest, ToPipe) {
  // Open the input file.
  const TempPath in_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());
  const FileDescriptor inf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(in_file.path(), O_RDWR));

  // Fill with some random data.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(inf.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));
  ASSERT_THAT(lseek(inf.get(), 0, SEEK_SET), SyscallSucceedsWithValue(0));

  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Splice to the pipe.
  EXPECT_THAT(splice(inf.get(), nullptr, wfd.get(), nullptr, kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // Contents should be equal.
  std::vector<char> rbuf(kPageSize);
  ASSERT_THAT(read(rfd.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data(), buf.size()), 0);
}

TEST(SpliceTest, ToPipeOffset) {
  // Open the input file.
  const TempPath in_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());
  const FileDescriptor inf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(in_file.path(), O_RDWR));

  // Fill with some random data.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(inf.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Splice to the pipe.
  loff_t in_offset = kPageSize / 2;
  EXPECT_THAT(
      splice(inf.get(), &in_offset, wfd.get(), nullptr, kPageSize / 2, 0),
      SyscallSucceedsWithValue(kPageSize / 2));

  // Contents should be equal to only the second part.
  std::vector<char> rbuf(kPageSize / 2);
  ASSERT_THAT(read(rfd.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize / 2));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data() + (kPageSize / 2), rbuf.size()), 0);
}

TEST(SpliceTest, FromPipe) {
  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Fill with some random data.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(wfd.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Open the input file.
  const TempPath out_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());
  const FileDescriptor outf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(out_file.path(), O_RDWR));

  // Splice to the output file.
  EXPECT_THAT(splice(rfd.get(), nullptr, outf.get(), nullptr, kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // The offset of the output should be equal to kPageSize. We assert that and
  // reset to zero so that we can read the contents and ensure they match.
  EXPECT_THAT(lseek(outf.get(), 0, SEEK_CUR),
              SyscallSucceedsWithValue(kPageSize));
  ASSERT_THAT(lseek(outf.get(), 0, SEEK_SET), SyscallSucceedsWithValue(0));

  // Contents should be equal.
  std::vector<char> rbuf(kPageSize);
  ASSERT_THAT(read(outf.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data(), buf.size()), 0);
}

TEST(SpliceTest, FromPipeOffset) {
  // Create a new pipe.
  int fds[2];
  ASSERT_THAT(pipe(fds), SyscallSucceeds());
  const FileDescriptor rfd(fds[0]);
  const FileDescriptor wfd(fds[1]);

  // Fill with some random data.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(wfd.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Open the input file.
  const TempPath out_file = ASSERT_NO_ERRNO_AND_VALUE(TempPath::CreateFile());
  const FileDescriptor outf =
      ASSERT_NO_ERRNO_AND_VALUE(Open(out_file.path(), O_RDWR));

  // Splice to the output file.
  loff_t out_offset = kPageSize / 2;
  EXPECT_THAT(splice(rfd.get(), nullptr, outf.get(), &out_offset, kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // Content should reflect the splice. We write to a specific offset in the
  // file, so the internals should now be allocated sparsely.
  std::vector<char> rbuf(kPageSize);
  ASSERT_THAT(read(outf.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  std::vector<char> zbuf(kPageSize / 2);
  memset(zbuf.data(), 0, zbuf.size());
  EXPECT_EQ(memcmp(rbuf.data(), zbuf.data(), zbuf.size()), 0);
  EXPECT_EQ(memcmp(rbuf.data() + kPageSize / 2, buf.data(), kPageSize / 2), 0);
}

TEST(SpliceTest, TwoPipes) {
  // Create two new pipes.
  int first[2], second[2];
  ASSERT_THAT(pipe(first), SyscallSucceeds());
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  ASSERT_THAT(pipe(second), SyscallSucceeds());
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // Fill with some random data.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ASSERT_THAT(write(wfd1.get(), buf.data(), buf.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Splice to the second pipe, using two operations.
  EXPECT_THAT(
      splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize / 2, 0),
      SyscallSucceedsWithValue(kPageSize / 2));
  EXPECT_THAT(
      splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize / 2, 0),
      SyscallSucceedsWithValue(kPageSize / 2));

  // Content should reflect the splice.
  std::vector<char> rbuf(kPageSize);
  ASSERT_THAT(read(rfd2.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data(), kPageSize), 0);
}

// Linux changed this behavior in ee5e001196d1345b8fee25925ff5f1d67936081e.
//
// Previously, blocking flags were not respected on pipes. Blocking flags are
// now respected on pipes as of kernel version 5.1+. In addition, blocking flags
// were mostly respected for sockets prior to version 5.1, but are less
// respected in version 5.1+.

struct BlockingParam {
  bool input_is_socket;
  int input_flags;
  bool output_is_socket;
  int output_flags;
  bool should_block;
  bool skip_on_old_linux;
  bool skip_on_new_linux;
};

class BlockingRead : public ::testing::TestWithParam<BlockingParam> {};

TEST_P(BlockingRead, Splice) {
  auto param = GetParam();

  if (!IsRunningOnGvisor()) {
    auto version = ASSERT_NO_ERRNO_AND_VALUE(GetKernelVersion());
    SKIP_IF(param.skip_on_old_linux &&
            (version.major < 5 || (version.major == 5 && version.minor < 1)));
    SKIP_IF(param.skip_on_new_linux &&
            (version.major > 5 || (version.major == 5 && version.minor >= 1)));
  }

  // Create two new pipes/sockets.
  int first[2], second[2];
  if (param.input_is_socket) {
    ASSERT_THAT(socketpair(AF_UNIX, SOCK_STREAM | param.input_flags, 0, first),
                SyscallSucceeds());
  } else {
    ASSERT_THAT(pipe2(first, param.input_flags), SyscallSucceeds());
  }
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  if (param.output_is_socket) {
    ASSERT_THAT(
        socketpair(AF_UNIX, SOCK_STREAM | param.output_flags, 0, second),
        SyscallSucceeds());
  } else {
    ASSERT_THAT(pipe2(second, param.output_flags), SyscallSucceeds());
  }
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // This thread writes to the input.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ScopedThread t([&]() {
    absl::SleepFor(absl::Milliseconds(100));
    ASSERT_THAT(write(wfd1.get(), buf.data(), buf.size()),
                SyscallSucceedsWithValue(kPageSize));
  });

  if (!param.should_block) {
    EXPECT_THAT(splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize, 0),
                SyscallFailsWithErrno(EWOULDBLOCK));

    // We're done.
    return;
  }

  // Attempt a splice immediately; it should block.
  EXPECT_THAT(splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // Thread should be joinable.
  t.Join();

  // Content should reflect the splice.
  std::vector<char> rbuf(kPageSize);
  EXPECT_THAT(fcntl(rfd2.get(), F_SETFL, O_NONBLOCK), SyscallSucceeds());
  ASSERT_THAT(read(rfd2.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data(), kPageSize), 0);
}

INSTANTIATE_TEST_SUITE_P(TestBlockingRead, BlockingRead,
                         ::testing::Values(
                             BlockingParam{
                                 false,  // input_is_socket
                                 0,      // input_flags
                                 false,  // output_is_socket
                                 0,      // output_flags
                                 true,   // should_block
                                 false,  // skip_on_old_linux
                                 false,  // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 O_NONBLOCK,  // input_flags
                                 false,       // output_is_socket
                                 0,           // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 0,           // input_flags
                                 false,       // output_is_socket
                                 O_NONBLOCK,  // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 O_NONBLOCK,  // input_flags
                                 false,       // output_is_socket
                                 O_NONBLOCK,  // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,   // input_is_socket
                                 0,      // input_flags
                                 false,  // output_is_socket
                                 0,      // output_flags
                                 true,   // should_block
                                 false,  // skip_on_old_linux
                                 false,  // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,           // input_is_socket
                                 SOCK_NONBLOCK,  // input_flags
                                 false,          // output_is_socket
                                 0,              // output_flags
                                 false,          // should_block
                                 false,          // skip_on_old_linux
                                 false,          // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,        // input_is_socket
                                 0,           // input_flags
                                 false,       // output_is_socket
                                 O_NONBLOCK,  // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,           // input_is_socket
                                 SOCK_NONBLOCK,  // input_flags
                                 false,          // output_is_socket
                                 O_NONBLOCK,     // output_flags
                                 false,          // should_block
                                 false,          // skip_on_old_linux
                                 false,          // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,  // input_is_socket
                                 0,      // input_flags
                                 true,   // output_is_socket
                                 0,      // output_flags
                                 true,   // should_block
                                 false,  // skip_on_old_linux
                                 false,  // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 O_NONBLOCK,  // input_flags
                                 true,        // output_is_socket
                                 0,           // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,          // input_is_socket
                                 0,              // input_flags
                                 true,           // output_is_socket
                                 SOCK_NONBLOCK,  // output_flags
                                 false,          // should_block
                                 true,           // skip_on_old_linux
                                 true,           // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,          // input_is_socket
                                 O_NONBLOCK,     // input_flags
                                 true,           // output_is_socket
                                 SOCK_NONBLOCK,  // output_flags
                                 false,          // should_block
                                 true,           // skip_on_old_linux
                                 false,          // skip_on_new_linux
                             }));

class BlockingWrite : public ::testing::TestWithParam<BlockingParam> {};

TEST_P(BlockingWrite, Splice) {
  auto param = GetParam();

  if (!IsRunningOnGvisor()) {
    auto version = ASSERT_NO_ERRNO_AND_VALUE(GetKernelVersion());
    SKIP_IF(param.skip_on_old_linux &&
            (version.major < 5 || (version.major == 5 && version.minor < 1)));
    SKIP_IF(param.skip_on_new_linux &&
            (version.major > 5 || (version.major == 5 && version.minor >= 1)));
  }

  // FIXME(gvisor.dev/issue/565): Splice will lose data if the write fails.
  SKIP_IF(param.should_block && IsRunningOnGvisor());

  // Create two new pipes/sockets.
  int first[2], second[2];
  if (param.input_is_socket) {
    ASSERT_THAT(socketpair(AF_UNIX, SOCK_STREAM | param.input_flags, 0, first),
                SyscallSucceeds());
  } else {
    ASSERT_THAT(pipe2(first, param.input_flags), SyscallSucceeds());
  }
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  if (param.output_is_socket) {
    ASSERT_THAT(
        socketpair(AF_UNIX, SOCK_STREAM | param.output_flags, 0, second),
        SyscallSucceeds());
  } else {
    ASSERT_THAT(pipe2(second, param.output_flags), SyscallSucceeds());
  }
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // Make some data available to be read.
  std::vector<char> buf1(kPageSize);
  RandomizeBuffer(buf1.data(), buf1.size());
  ASSERT_THAT(write(wfd1.get(), buf1.data(), buf1.size()),
              SyscallSucceedsWithValue(kPageSize));

  int pipe_size = 0;
  if (param.output_is_socket) {
    std::vector<char> buf(100);
    for (;;) {
      int ret = send(wfd2.get(), buf.data(), buf.size(), MSG_DONTWAIT);
      if (ret > 0) {
        pipe_size += ret;
        continue;
      }
      if (errno == EWOULDBLOCK) {
        break;
      }
      ASSERT_THAT(ret, SyscallSucceeds());
    }
  } else {
    // Fill up the write pipe's buffer.
    ASSERT_THAT(pipe_size = fcntl(wfd2.get(), F_GETPIPE_SZ), SyscallSucceeds());
    std::vector<char> buf(pipe_size);
    ASSERT_THAT(write(wfd2.get(), buf.data(), buf.size()),
                SyscallSucceedsWithValue(pipe_size));
  }

  ScopedThread t([&]() {
    absl::SleepFor(absl::Milliseconds(100));
    std::vector<char> buf(pipe_size);
    ASSERT_THAT(read(rfd2.get(), buf.data(), buf.size()),
                SyscallSucceedsWithValue(pipe_size));
  });

  if (!param.should_block) {
    EXPECT_THAT(splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize, 0),
                SyscallFailsWithErrno(EWOULDBLOCK));

    // We're done.
    return;
  }

  // Attempt a splice immediately; it should block.
  EXPECT_THAT(splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // Thread should be joinable.
  t.Join();

  // Content should reflect the splice.
  std::vector<char> rbuf(kPageSize);
  EXPECT_THAT(fcntl(rfd2.get(), F_SETFL, O_NONBLOCK), SyscallSucceeds());
  ASSERT_THAT(read(rfd2.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf1.data(), kPageSize), 0);
}

INSTANTIATE_TEST_SUITE_P(TestBlockingWrite, BlockingWrite,
                         ::testing::Values(
                             BlockingParam{
                                 false,  // input_is_socket
                                 0,      // input_flags
                                 false,  // output_is_socket
                                 0,      // output_flags
                                 true,   // should_block
                                 false,  // skip_on_old_linux
                                 false,  // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 O_NONBLOCK,  // input_flags
                                 false,       // output_is_socket
                                 0,           // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 0,           // input_flags
                                 false,       // output_is_socket
                                 O_NONBLOCK,  // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 O_NONBLOCK,  // input_flags
                                 false,       // output_is_socket
                                 O_NONBLOCK,  // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,   // input_is_socket
                                 0,      // input_flags
                                 false,  // output_is_socket
                                 0,      // output_flags
                                 true,   // should_block
                                 true,   // skip_on_old_linux
                                 false,  // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,           // input_is_socket
                                 SOCK_NONBLOCK,  // input_flags
                                 false,          // output_is_socket
                                 0,              // output_flags
                                 false,          // should_block
                                 true,           // skip_on_old_linux
                                 true,           // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,        // input_is_socket
                                 0,           // input_flags
                                 false,       // output_is_socket
                                 O_NONBLOCK,  // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 false,       // skip_on_new_linux
                             },
                             BlockingParam{
                                 true,           // input_is_socket
                                 SOCK_NONBLOCK,  // input_flags
                                 false,          // output_is_socket
                                 O_NONBLOCK,     // output_flags
                                 false,          // should_block
                                 true,           // skip_on_old_linux
                                 false,          // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,  // input_is_socket
                                 0,      // input_flags
                                 true,   // output_is_socket
                                 0,      // output_flags
                                 true,   // should_block
                                 false,  // skip_on_old_linux
                                 false,  // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,       // input_is_socket
                                 O_NONBLOCK,  // input_flags
                                 true,        // output_is_socket
                                 0,           // output_flags
                                 false,       // should_block
                                 true,        // skip_on_old_linux
                                 true,        // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,          // input_is_socket
                                 0,              // input_flags
                                 true,           // output_is_socket
                                 SOCK_NONBLOCK,  // output_flags
                                 false,          // should_block
                                 true,           // skip_on_old_linux
                                 true,           // skip_on_new_linux
                             },
                             BlockingParam{
                                 false,          // input_is_socket
                                 O_NONBLOCK,     // input_flags
                                 true,           // output_is_socket
                                 SOCK_NONBLOCK,  // output_flags
                                 false,          // should_block
                                 true,           // skip_on_old_linux
                                 true,           // skip_on_new_linux
                             }));

TEST(TeeTest, BlockingRead) {
  SKIP_IF(IsRunningOnGvisor());

  // Create two new pipes.
  int first[2], second[2];
  ASSERT_THAT(pipe(first), SyscallSucceeds());
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  ASSERT_THAT(pipe(second), SyscallSucceeds());
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // This thread writes to the main pipe.
  std::vector<char> buf(kPageSize);
  RandomizeBuffer(buf.data(), buf.size());
  ScopedThread t([&]() {
    absl::SleepFor(absl::Milliseconds(100));
    ASSERT_THAT(write(wfd1.get(), buf.data(), buf.size()),
                SyscallSucceedsWithValue(kPageSize));
  });

  // Attempt a tee immediately; it should block.
  EXPECT_THAT(tee(rfd1.get(), wfd2.get(), kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // Thread should be joinable.
  t.Join();

  // Content should reflect the tee, in both pipes.
  std::vector<char> rbuf(kPageSize);
  ASSERT_THAT(read(rfd2.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data(), kPageSize), 0);
  ASSERT_THAT(read(rfd1.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf.data(), kPageSize), 0);
}

TEST(TeeTest, BlockingWrite) {
  SKIP_IF(IsRunningOnGvisor());

  // Create two new pipes.
  int first[2], second[2];
  ASSERT_THAT(pipe(first), SyscallSucceeds());
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  ASSERT_THAT(pipe(second), SyscallSucceeds());
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // Make some data available to be read.
  std::vector<char> buf1(kPageSize);
  RandomizeBuffer(buf1.data(), buf1.size());
  ASSERT_THAT(write(wfd1.get(), buf1.data(), buf1.size()),
              SyscallSucceedsWithValue(kPageSize));

  // Fill up the write pipe's buffer.
  int pipe_size = -1;
  ASSERT_THAT(pipe_size = fcntl(wfd2.get(), F_GETPIPE_SZ), SyscallSucceeds());
  std::vector<char> buf2(pipe_size);
  ASSERT_THAT(write(wfd2.get(), buf2.data(), buf2.size()),
              SyscallSucceedsWithValue(pipe_size));

  ScopedThread t([&]() {
    absl::SleepFor(absl::Milliseconds(100));
    ASSERT_THAT(read(rfd2.get(), buf2.data(), buf2.size()),
                SyscallSucceedsWithValue(pipe_size));
  });

  // Attempt a tee immediately; it should block.
  EXPECT_THAT(tee(rfd1.get(), wfd2.get(), kPageSize, 0),
              SyscallSucceedsWithValue(kPageSize));

  // Thread should be joinable.
  t.Join();

  // Content should reflect the tee.
  std::vector<char> rbuf(kPageSize);
  ASSERT_THAT(read(rfd2.get(), rbuf.data(), rbuf.size()),
              SyscallSucceedsWithValue(kPageSize));
  EXPECT_EQ(memcmp(rbuf.data(), buf1.data(), kPageSize), 0);
}

TEST(SpliceTest, NonBlocking) {
  // Create two new pipes.
  int first[2], second[2];
  ASSERT_THAT(pipe(first), SyscallSucceeds());
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  ASSERT_THAT(pipe(second), SyscallSucceeds());
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // Splice with no data to back it.
  EXPECT_THAT(splice(rfd1.get(), nullptr, wfd2.get(), nullptr, kPageSize,
                     SPLICE_F_NONBLOCK),
              SyscallFailsWithErrno(EAGAIN));
}

TEST(TeeTest, NonBlocking) {
  SKIP_IF(IsRunningOnGvisor());

  // Create two new pipes.
  int first[2], second[2];
  ASSERT_THAT(pipe(first), SyscallSucceeds());
  const FileDescriptor rfd1(first[0]);
  const FileDescriptor wfd1(first[1]);
  ASSERT_THAT(pipe(second), SyscallSucceeds());
  const FileDescriptor rfd2(second[0]);
  const FileDescriptor wfd2(second[1]);

  // Splice with no data to back it.
  EXPECT_THAT(tee(rfd1.get(), wfd2.get(), kPageSize, SPLICE_F_NONBLOCK),
              SyscallFailsWithErrno(EAGAIN));
}

}  // namespace

}  // namespace testing
}  // namespace gvisor