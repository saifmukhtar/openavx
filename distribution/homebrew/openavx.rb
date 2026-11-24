class Openavx < Formula
  desc "Open Audio-Video eXchange (OpenAVX) C99 high-performance IP streaming suite"
  homepage "https://github.com/saifmukhtar/openavx"
  url "https://github.com/saifmukhtar/openavx/archive/refs/tags/v0.2.1.tar.gz"
  sha256 "SKIP" # TODO: set tarball sha256 once v0.2.1 tag is published
  license "AGPL-3.0-or-later WITH Classpath-exception-2.0"
  head "https://github.com/saifmukhtar/openavx.git", branch: "main"

  depends_on "cmake" => :build

  def install
    args = std_cmake_args + %w[
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_TESTING=ON
    ]

    system "cmake", "-S", ".", "-B", "build", *args
    system "cmake", "--build", "build"
    system "ctest", "--test-dir", "build", "--output-on-failure"
    system "cmake", "--install", "build"
  end

  service do
    run [opt_bin/"openavx-discovery-server", "--port", "7959"]
    keep_alive true
    log_path var/"log/openavx-discovery-server.log"
    error_log_path var/"log/openavx-discovery-server.err"
  end

  test do
    assert_match "OpenAVX CLI", shell_output("#{bin}/openavx --help")
    assert_match "OpenAVX Network Source Finder", shell_output("#{bin}/openavx-find --help")
    assert_match "OpenAVX Test Pattern Generator", shell_output("#{bin}/openavx-test-pattern --help")
    assert_match "OpenAVX Web & RTSP Gateway", shell_output("#{bin}/openavx-gateway --help")
    assert_match "OpenAVX Standalone Discovery Server Daemon", shell_output("#{bin}/openavx-discovery-server --help")

    (testpath/"test.c").write <<~EOS
      #include <openavx.h>
      #include <stdio.h>
      int main(void) {
        printf("OpenAVX version: %s\\n", oavx_version());
        return 0;
      }
    EOS
    system ENV.cc, "test.c", "-I#{include}", "-L#{lib}", "-Wl,-rpath,#{lib}", "-lopenavx", "-o", "test"
    assert_match "openavx v0.2.1", shell_output("./test")
  end
end
