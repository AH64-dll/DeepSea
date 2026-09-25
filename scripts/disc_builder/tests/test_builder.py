"""Synthetic fixtures only: no retail game data is needed by these tests."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from archives import yaz0, rarc_files
from build import Builder, MODULE_NAME


class ArchiveTests(unittest.TestCase):
    def test_yaz0_overlapping_reference(self):
        # Literal A followed by five overlapping copies from distance one.
        data = b"Yaz0" + struct.pack(">I", 6) + bytes(8) + b"\x80A\x30\x00"
        self.assertEqual(yaz0(data), b"AAAAAA")

    def test_yaz0_rejects_bad_reference_and_truncation(self):
        for payload in (b"\x00\x10\x00", b"\x80", b"\x00\x00\x00"):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                yaz0(b"Yaz0" + struct.pack(">I", 6) + bytes(8) + payload)

    def test_yaz0_rejects_oversized_output(self):
        with self.assertRaises(ValueError):
            yaz0(b"Yaz0" + struct.pack(">I", 0xffffffff) + bytes(8))

    def test_rarc_rejects_out_of_bounds_tables(self):
        data = bytearray(64)
        data[:4] = b"RARC"
        struct.pack_into(">I", data, 4, len(data))
        struct.pack_into(">I", data, 0x20, 0xffffffff)
        with self.assertRaises(ValueError):
            list(rarc_files(bytes(data)))


class FakeBuilder(Builder):
    def __init__(self, output):
        self.args = SimpleNamespace(output=output, rebuild=False)
        self.spec = {"inputs": {"sys/main.dol": "synthetic"}}
        self.started = 0
        self.work = None
        self.generated = 0
        self.fail = False
        self.states = []

    def validate(self):
        return Path("synthetic")

    def recipe(self):
        return {"recipe": "one"}

    def progress(self, stage, completed=0, total=1, **extra):
        self.states.append((stage, extra))

    def generate(self, root):
        self.generated += 1
        (self.work / "dol/generated").mkdir(parents=True)
        (self.work / "dol/generated/chunk.c").write_text("int translated;")
        (self.work / "objects.rsp").write_text("chunk.o")
        (self.work / "generate-dol.log").write_text("log")
        if self.fail:
            raise RuntimeError("simulated compiler error")

    def compile(self):
        path = self.work / MODULE_NAME
        path.write_bytes(b"synthetic game module")
        return path


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="disc builder é ")
        self.root = Path(self.temp.name)
        self.builder = FakeBuilder(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def active(self):
        return Path((self.root / "GZLE01/active-module.txt").read_text(encoding="utf-8").strip())

    def test_second_launch_reuses_verified_build(self):
        self.builder.build()
        original = self.active()
        self.builder.build()
        self.assertEqual(self.builder.generated, 1)
        self.assertEqual(self.active(), original)
        self.assertTrue(self.builder.states[-1][1]["cache_hit"])

    def test_tampered_dll_is_rebuilt_and_then_cached(self):
        self.builder.build()
        self.active().write_bytes(b"corrupt")
        self.builder.build()
        self.builder.build()
        self.assertEqual(self.builder.generated, 2)
        self.assertEqual(self.active().read_bytes(), b"synthetic game module")

    def test_corrupt_receipt_is_rebuilt(self):
        self.builder.build()
        self.active().with_name("build.json").write_text("broken json")
        self.builder.build()
        self.assertEqual(self.builder.generated, 2)

    def test_failed_rebuild_preserves_active_module(self):
        self.builder.build()
        original = self.active()
        self.builder.args.rebuild = True
        self.builder.fail = True
        with self.assertRaises(RuntimeError):
            self.builder.build()
        self.assertEqual(self.active(), original)
        self.assertEqual(original.read_bytes(), b"synthetic game module")

    def test_intermediates_are_removed_logs_and_receipt_kept(self):
        self.builder.build()
        artifact = self.active().parent
        self.assertFalse((artifact / "dol").exists())
        self.assertFalse((artifact / "objects.rsp").exists())
        self.assertTrue((artifact / "generate-dol.log").is_file())
        self.assertTrue((artifact / "build.json").is_file())

    def test_failed_attempt_is_kept_until_the_next_attempt(self):
        self.builder.fail = True
        with self.assertRaises(RuntimeError):
            self.builder.build()
        failed = self.builder.work
        self.assertTrue((failed / "generate-dol.log").is_file())
        self.builder.fail = False
        self.builder.build()
        self.assertFalse(failed.exists())
        self.assertEqual(self.active().read_bytes(), b"synthetic game module")

    def test_wrong_game_is_rejected_before_compilation(self):
        game = self.root / "game"
        (game / "sys").mkdir(parents=True)
        (game / "sys/main.dol").write_bytes(b"wrong revision")
        builder = self.builder
        builder.args.game_root = game
        builder.spec = {"inputs": {"sys/main.dol": hashlib.sha256(b"correct revision").hexdigest()}}
        with self.assertRaisesRegex(ValueError, "Unsupported or damaged"):
            Builder.validate(builder)
        self.assertEqual(builder.generated, 0)


class RelcallTests(unittest.TestCase):
    # Synthetic chunk: one call with an in-chunk continuation label, one
    # call whose continuation label is missing, one bctr tail jump.
    CHUNK = (
        "void func_91000000(CPUState* ctx) {\n"
        "label_91000000:\n"
        "            ctx->lr = 0x91000008u;\n"
        "            ctx->pc = 0x80241178u;\n"
        "            return;\n"
        "label_91000008:\n"
        "            ctx->lr = 0x9100FFF0u;\n"
        "            ctx->pc = 0x80241178u;\n"
        "            return;\n"
        "        ctx->pc = target;\n"
        "        return;\n"
        "}\n")

    def test_call_and_jump_sites_get_epoch_pinned_caches(self):
        from relcall import transform_source
        out, calls, jumps, skipped = transform_source(self.CHUNK)
        self.assertEqual((calls, jumps, skipped), (1, 1, 1))
        # cached call resumes the continuation label in-chunk
        self.assertIn("static u32 s_ict_rc1, s_icp_rc1, s_icdg_rc1, s_icrg_rc1, s_icg2_rc1;", out)
        self.assertIn("goto label_91000008;", out)
        self.assertIn("s_icrg_rc1 == g_rel_dispatch_gen", out)
        # the call without a continuation label is left exactly as emitted
        self.assertIn("ctx->lr = 0x9100FFF0u;\n            ctx->pc = 0x80241178u;\n            return;", out)
        # the tail jump is numbered after the calls
        self.assertIn("static u32 s_ijt_rc2, s_ijp_rc2, s_ijdg_rc2, s_ijrg_rc2;", out)
        # idempotent in effect: a second pass finds no bare sites
        again = transform_source(out)
        self.assertEqual(again[1:], (0, 0, 1))


class ManifestTests(unittest.TestCase):
    def test_line_endings_and_table_comments_are_ignored_code_is_not(self):
        import source_manifest
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            for d in ("dol/generated/chunks", "rel/generated/rels/m_1/chunks", "tables"):
                (work / d).mkdir(parents=True)
            (work / "dol/generated/chunks/c.c").write_bytes(b"int a;\r\nint b;\r\n")
            (work / "rel/generated/rels/m_1/chunks/r.c").write_bytes(b"int r;\n")
            (work / "tables/t.inc").write_bytes(b"// built in C:\\one\n{1,2}\n")
            manifest = work / "m.json"
            manifest.write_text(json.dumps({"files": source_manifest.collect(work)}))
            # another machine: LF endings, different local path in a comment
            (work / "dol/generated/chunks/c.c").write_bytes(b"int a;\nint b;\n")
            (work / "tables/t.inc").write_bytes(b"// built in D:\\two\n{1,2}\n")
            self.assertEqual(source_manifest.verify(work, manifest), [])
            # a code change is caught, as is an unexpected extra file
            (work / "rel/generated/rels/m_1/chunks/r.c").write_bytes(b"int r2;\n")
            (work / "dol/generated/chunks/extra.c").write_bytes(b"\n")
            self.assertEqual(source_manifest.verify(work, manifest),
                             ["dol/chunks/extra.c", "rel/m_1/chunks/r.c"])


class PatchTests(unittest.TestCase):
    def test_strict_apply_and_mismatch(self):
        from patching import apply_unified_patch
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "f.h").write_text("a\nb\nc\n", newline="\r\n")
            (tmp / "p.patch").write_text("--- a/f.h\n+++ b/f.h\n@@ -1,3 +1,4 @@\n a\n-b\n+B\n+B2\n c\n")
            apply_unified_patch(tmp / "f.h", tmp / "p.patch")
            self.assertEqual((tmp / "f.h").read_bytes(), b"a\nB\nB2\nc\n")
            with self.assertRaisesRegex(ValueError, "does not match"):
                apply_unified_patch(tmp / "f.h", tmp / "p.patch")


if __name__ == "__main__":
    unittest.main()
