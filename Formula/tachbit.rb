# Homebrew: brew tap superbro456/tachbit https://github.com/superbro456/Tachbit
#           brew install --HEAD tachbit
class Tachbit < Formula
  desc "Clear language for real fast programs, games and systems"
  homepage "https://github.com/superbro456/Tachbit"
  license "MIT"
  head "https://github.com/superbro456/Tachbit.git", branch: "main"

  def install
    system "bash", "./tools/build_tb.sh"
    libexec.install "build/tb"
    (pkgshare/"lib").install Dir["lib/*.tb"]
    pkgshare.install "examples"
    (bin/"tb").write_env_script libexec/"tb", TB_LIB: pkgshare/"lib"
  end

  test do
    (testpath/"hello.tb").write <<~TB
      func main()
          print(6 * 7)
    TB
    assert_match "42", shell_output("#{bin}/tb run #{testpath}/hello.tb")
  end
end
