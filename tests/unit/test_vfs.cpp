#include <filesystem>
#include <fstream>

#include "runtime/vfs.h"
#include "test.h"

using namespace xbem;
namespace fs = std::filesystem;

namespace {
fs::path setup_tree() {
  const fs::path root = fs::temp_directory_path() / "xbem-unit" / "vfs";
  fs::remove_all(root);
  fs::create_directories(root / "pkg" / "Content" / "Levels");
  fs::create_directories(root / "scratch");
  std::ofstream(root / "pkg" / "Content" / "Levels" / "Level01.PAK") << "x";
  return root;
}
}  // namespace

TEST(vfs_resolves_case_insensitively) {
  const fs::path root = setup_tree();
  rt::Vfs vfs;
  vfs.mount('G', root / "pkg", true);
  vfs.mount('t', root / "scratch", false);

  auto r = vfs.resolve(std::string_view("G:\\content\\levels\\level01.pak"));
  CHECK(r.has_value());
  if (r) {
    CHECK(fs::exists(r->host_path));
    CHECK_EQ(r->host_path.filename().string(), std::string("Level01.PAK"));
    CHECK(r->read_only);
  }
  auto t = vfs.resolve(std::u16string_view(u"T:/saves/slot1.sav"));
  CHECK(t.has_value() && !t->read_only);
  if (t) CHECK_EQ(t->host_path, (fs::absolute(root / "scratch") / "saves" / "slot1.sav").lexically_normal());
}

TEST(vfs_relative_and_rooted_paths_use_default_drive) {
  const fs::path root = setup_tree();
  rt::Vfs vfs;
  vfs.mount('G', root / "pkg", true);
  auto a = vfs.resolve(std::string_view("Content\\Levels"));
  auto b = vfs.resolve(std::string_view("\\CONTENT\\levels"));
  auto c = vfs.resolve(std::string_view("\\\\?\\G:\\content\\levels"));
  CHECK(a && b && c);
  if (a && b && c) {
    CHECK(fs::is_directory(a->host_path));
    CHECK_EQ(a->host_path, b->host_path);
    CHECK_EQ(a->host_path, c->host_path);
  }
}

TEST(vfs_dotdot_cannot_escape_mount) {
  const fs::path root = setup_tree();
  rt::Vfs vfs;
  vfs.mount('G', root / "pkg", true);
  auto r = vfs.resolve(std::string_view("G:\\..\\..\\..\\etc\\passwd"));
  CHECK(r.has_value());
  if (r) CHECK_EQ(r->host_path, (fs::absolute(root / "pkg") / "etc" / "passwd").lexically_normal());
  CHECK(!vfs.resolve(std::string_view("Q:\\file")).has_value());  // unmounted drive
}

TEST(vfs_to_guest) {
  const fs::path root = setup_tree();
  rt::Vfs vfs;
  vfs.mount('G', root / "pkg", true);
  CHECK_EQ(vfs.to_guest(root / "pkg" / "Content" / "x.bin").value_or(""), std::string("G:\\Content\\x.bin"));
  CHECK_EQ(vfs.to_guest(root / "pkg").value_or(""), std::string("G:\\"));
  CHECK(!vfs.to_guest(root / "elsewhere").has_value());
}
