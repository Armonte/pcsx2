// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/GameRollback.h"
#include "Sdbz/EeHooks.h"
#include "Sdbz/NetBridge.h"
#include "Sdbz/RollbackDevice.h"

#include "DebugTools/BiosDebugData.h"
#include "Memory.h"
#include "R5900.h"
#include "ps2/BiosTools.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/YAML.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameRollback
{
	namespace
	{
		constexpr u32 RAM_MASK = 0x01FFFFFFu;
		constexpr u32 NONE = 0xFFFFFFFFu;

		u32 Rd(u32 a);
		u32 Rd16(u32 a);
		u32 Rd8(u32 a);

		// A number, or an address expression evaluated on EE memory: u8/u16/u32[e], + - * & |, == != < <= > >=, && ||,
		// unary -, parentheses, the loop index `i`, and one-parameter macros from the manifest (`macros: {ph: ...x...}`).
		struct Val
		{
			bool is_expr = false;
			u32 v = 0;
			std::string e;
		};
		std::unordered_map<std::string, std::string> s_macros; // current manifest's macros (EE thread)

		class ExprParser
		{
		public:
			ExprParser(const std::string& text, s64 i, s64 x) : m_p(text.c_str()), m_i(i), m_x(x) {}
			bool Parse(s64* out)
			{
				const s64 v = Or();
				Skip();
				if (*m_p != 0)
					m_ok = false;
				*out = v;
				return m_ok;
			}

		private:
			const char* m_p;
			s64 m_i, m_x;
			bool m_ok = true;
			int m_depth = 0;

			void Skip()
			{
				while (*m_p == ' ' || *m_p == '\t')
					m_p++;
			}
			bool Eat(const char* tok)
			{
				Skip();
				const size_t n = std::strlen(tok);
				if (std::strncmp(m_p, tok, n) != 0)
					return false;
				m_p += n;
				return true;
			}
			s64 Or()
			{
				s64 v = And();
				while (Eat("||"))
					v = (And() != 0) || (v != 0);
				return v;
			}
			s64 And()
			{
				s64 v = Cmp();
				while (Eat("&&"))
					v = (Cmp() != 0) && (v != 0);
				return v;
			}
			s64 Cmp()
			{
				s64 v = BitOr();
				for (;;)
				{
					if (Eat("=="))
						v = v == BitOr();
					else if (Eat("!="))
						v = v != BitOr();
					else if (Eat("<="))
						v = v <= BitOr();
					else if (Eat(">="))
						v = v >= BitOr();
					else if (Eat("<"))
						v = v < BitOr();
					else if (Eat(">"))
						v = v > BitOr();
					else
						return v;
				}
			}
			s64 BitOr()
			{
				s64 v = BitAnd();
				for (;;)
				{
					Skip();
					if (m_p[0] == '|' && m_p[1] != '|')
					{
						m_p++;
						v |= BitAnd();
					}
					else
						return v;
				}
			}
			s64 BitAnd()
			{
				s64 v = Add();
				for (;;)
				{
					Skip();
					if (m_p[0] == '&' && m_p[1] != '&')
					{
						m_p++;
						v &= Add();
					}
					else
						return v;
				}
			}
			s64 Add()
			{
				s64 v = Mul();
				for (;;)
				{
					if (Eat("+"))
						v += Mul();
					else if (Eat("-"))
						v -= Mul();
					else
						return v;
				}
			}
			s64 Mul()
			{
				s64 v = Unary();
				while (Eat("*"))
					v *= Unary();
				return v;
			}
			s64 Unary()
			{
				if (Eat("-"))
					return -Unary();
				return Primary();
			}
			s64 Load(int bytes)
			{
				if (!Eat("["))
				{
					m_ok = false;
					return 0;
				}
				const s64 a = Or();
				if (!Eat("]") || a < 0 || a >= 0x02000000)
				{
					m_ok = false;
					return 0;
				}
				const u32 ua = static_cast<u32>(a);
				return bytes == 4 ? Rd(ua) : bytes == 2 ? Rd16(ua) : Rd8(ua);
			}
			s64 Primary()
			{
				Skip();
				if (Eat("("))
				{
					const s64 v = Or();
					if (!Eat(")"))
						m_ok = false;
					return v;
				}
				if (std::isdigit(static_cast<unsigned char>(*m_p)))
				{
					char* end = nullptr;
					const s64 v = std::strtoll(m_p, &end, 0);
					m_p = end;
					return v;
				}
				std::string id;
				while (std::isalnum(static_cast<unsigned char>(*m_p)) || *m_p == '_')
					id += *m_p++;
				if (id == "u32")
					return Load(4);
				if (id == "u16")
					return Load(2);
				if (id == "u8")
					return Load(1);
				if (id == "i")
					return m_i;
				if (id == "x")
					return m_x;
				const auto it = s_macros.find(id);
				if (it != s_macros.end() && Eat("(") && m_depth < 8)
				{
					const s64 arg = Or();
					if (!Eat(")"))
						m_ok = false;
					ExprParser sub(it->second, m_i, arg);
					sub.m_depth = m_depth + 1;
					s64 v = 0;
					if (!sub.Parse(&v))
						m_ok = false;
					return v;
				}
				m_ok = false;
				return 0;
			}
		};
		bool Eval(const Val& val, s64 i, s64* out)
		{
			if (!val.is_expr)
			{
				*out = val.v;
				return true;
			}
			ExprParser p(val.e, i, 0);
			return p.Parse(out);
		}

		// ---------------------------------------------------------------------------------------------------------
		// manifest
		// ---------------------------------------------------------------------------------------------------------
		struct Range
		{
			u32 addr = 0, len = 0;
		};
		struct TableSpec // table of {.., used flag at used_off, pointer at ptr_off, ..} entries
		{
			u32 base = 0, count = 0, stride = 0, used_off = NONE, ptr_off = 0;
		};
		struct ListSpec // circular list of blocks: next pointer at next_off, ends back at the head
		{
			u32 head = 0, next_off = 0, max = 64;
			u32 first = 0, stride = 0, count = 0, extra_ptr = 0; // entries inside each block (+ *extra_ptr)
		};
		// A fixed address ([addr]) or a pointer chain [base, o1, ..., on]: p = *base; p = *(p + ok) for the middle
		// offsets; the address is p + on. E.g. [0x51E444, 0x334, 0] = the RwCamera the camera object points to.
		struct AddrSpec
		{
			std::vector<u32> chain;
		};
		enum class DynKind
		{
			Exclude,
			ExcludeResimRestore,
		};
		struct Dynamic
		{
			DynKind kind = DynKind::Exclude;
			std::optional<TableSpec> table;
			std::optional<ListSpec> list;
			std::optional<AddrSpec> chain;
			std::vector<Range> fields; // offset + length inside each object (list: inside each entry)
			std::vector<u32> lens;     // per table index: length of field 0 (overrides fields[0].len)
			u32 len = 0;               // chain: length
			std::optional<Val> expr;   // expression form: address of object i (i < count), length len_val
			Val count{false, 1, {}};
			Val len_val;
		};
		struct RegionSpec
		{
			u32 addr = 0;
			Val len;
			bool has_end = false;
			Val end;
		};
		struct Item // static state item, fixed or via a pointer chain (resolved when rollback starts)
		{
			AddrSpec where;
			u32 len = 0;
			std::string name;
		};
		enum class StepKind
		{
			Call,
			CallEach,
			Fill32,
			Copy32,
			Age,
		};
		struct Op
		{
			StepKind kind = StepKind::Fill32;
			u32 addr = 0, count = 0, stride = 4, value = 0; // fill32
			u32 from = 0, to = 0;                           // copy32
			u32 id_off = 0, age_off = 4, limit = 0, if_nonzero = 0; // age: if (*if_nonzero) for each: id>0 && ++age>=limit -> id=0
		};
		struct Step
		{
			StepKind kind = StepKind::Call;
			u32 fn = 0;
			bool has_a0 = false;
			u32 a0 = 0;
			TableSpec each;
			Op op;
		};
		struct EntryAction
		{
			u32 at = 0;
			bool resim_only = true;
			bool ret = true;
			std::vector<Op> ops;
		};
		struct VsLen
		{
			u32 frames = 0, loop_start = 0, loop_end = 0;
			bool loops = false;
		};
		struct VsHook
		{
			std::string kind; // tick request kill pause resume fade query post
			u32 at = 0;
			std::vector<u32> ra;
			int ch_reg = 4, idx_reg = 5, prio_reg = 7, paused_reg = 8, dur_reg = 5, target_reg = 112;
			u32 ch_mask = 0xFFFFFFFFu;
			bool need_ready = false;
			std::string value;
			s64 not_ready = 0;
		};
		struct VirtualStreams
		{
			u32 state = 0, channels = 4, k_prep = 13, ready_addr = 0;
			std::unordered_map<u64, VsLen> lengths; // (partition << 32 | index)
			std::vector<VsHook> hooks;
		};
		struct Manifest
		{
			std::string serial, name;
			std::vector<RegionSpec> regions;
			std::unordered_map<std::string, std::string> macros;
			std::optional<Val> gate_when;
			std::unordered_map<u32, u32> gate_values; // resim gates that also set $v0
			bool exclude_thread_stacks = true;
			VirtualStreams vs;
			bool pad_record_replay = false;
			std::vector<Item> excludes, ignores, watches;
			std::vector<Range> gate_stable, resim_restore;
			std::vector<Dynamic> dynamic;
			u32 gate_counter = 0;
			Range input_block;
			Range rng_split;
			u32 sim_tick_site = 0, return_addr = 0, render_begin_site = 0, render_end_site = 0;
			std::vector<Step> steps;
			std::vector<u32> resim_gates, resim_skips, always_skips;
			std::vector<EntryAction> entry_actions;
			u32 pad_read_fn = 0, pad_site = 0;
			u32 pad_mode = 0x73; // report mode byte every netplay peer builds reports with (the game's configured pad mode)
			// optional separate record/replay point (a higher-level read whose output is replayed in resim, e.g. the
			// game's port-state read that wraps the pad library): player = sum of player_regs, output at buf_reg
			u32 replay_fn = 0, replay_site = 0, replay_len = 32;
			std::vector<int> replay_player_regs;
			int replay_buf_reg = 5;
			u32 rng_float = 0, rng_int = 0;
			u32 sound_seed = 0; // EE word holding the sound-only RNG stream (rolled back; same start on every peer)
			std::unordered_set<u32> sound_sites;
			std::unordered_map<u32, u32> trace_ids; // function -> trace id
		};

		u32 ParseU32(const ryml::ConstNodeRef& n)
		{
			const ryml::csubstr v = n.val();
			const std::string s(v.str, v.len);
			return static_cast<u32>(std::strtoll(s.c_str(), nullptr, 0));
		}
		Val ParseVal(const ryml::ConstNodeRef& n)
		{
			const ryml::csubstr v = n.val();
			const std::string s(v.str, v.len);
			char* end = nullptr;
			const long long num = std::strtoll(s.c_str(), &end, 0);
			if (!s.empty() && end && *end == 0)
				return {false, static_cast<u32>(num), {}};
			return {true, 0, s};
		}
		u32 Get(const ryml::ConstNodeRef& n, const char* key, u32 def)
		{
			if (!n.is_map() || !n.has_child(ryml::to_csubstr(key)))
				return def;
			return ParseU32(n[ryml::to_csubstr(key)]);
		}
		std::string GetStr(const ryml::ConstNodeRef& n, const char* key)
		{
			if (!n.is_map() || !n.has_child(ryml::to_csubstr(key)))
				return {};
			const ryml::csubstr v = n[ryml::to_csubstr(key)].val();
			return std::string(v.str, v.len);
		}
		bool Has(const ryml::ConstNodeRef& n, const char* key) { return n.is_map() && n.has_child(ryml::to_csubstr(key)); }
		ryml::ConstNodeRef Child(const ryml::ConstNodeRef& n, const char* key) { return n[ryml::to_csubstr(key)]; }

		// [addr, len] | {addr:, len:} -> Range
		Range ParseRange(const ryml::ConstNodeRef& n)
		{
			if (n.is_seq())
				return {ParseU32(n[0]), n.num_children() > 1 ? ParseU32(n[1]) : 0};
			return {Get(n, "addr", 0), Get(n, "len", 0)};
		}
		std::vector<Range> ParseRanges(const ryml::ConstNodeRef& n)
		{
			std::vector<Range> out;
			for (const ryml::ConstNodeRef& c : n.children())
				out.push_back(ParseRange(c));
			return out;
		}
		AddrSpec ParseChain(const ryml::ConstNodeRef& n)
		{
			AddrSpec a;
			for (const ryml::ConstNodeRef& c : n.children())
				a.chain.push_back(ParseU32(c));
			return a;
		}
		// [addr, len, "why"] | {addr:, len:, name:} | {chain: [...], len:, name:}
		Item ParseItem(const ryml::ConstNodeRef& n)
		{
			Item it;
			if (n.is_seq())
			{
				it.where.chain = {ParseU32(n[0])};
				it.len = n.num_children() > 1 ? ParseU32(n[1]) : 0;
				if (n.num_children() > 2)
					it.name = std::string(n[2].val().str, n[2].val().len);
				return it;
			}
			it.where = Has(n, "chain") ? ParseChain(Child(n, "chain")) : AddrSpec{{Get(n, "addr", 0)}};
			it.len = Get(n, "len", 0);
			it.name = GetStr(n, "name");
			return it;
		}
		TableSpec ParseTable(const ryml::ConstNodeRef& n)
		{
			return {Get(n, "base", 0), Get(n, "count", 0), Get(n, "stride", 0), Get(n, "used_off", NONE), Get(n, "ptr_off", 0)};
		}
		Op ParseOp(const ryml::ConstNodeRef& n)
		{
			Op op;
			if (Has(n, "fill32"))
			{
				const auto f = Child(n, "fill32");
				op.kind = StepKind::Fill32;
				op.addr = Get(f, "addr", 0);
				op.count = Get(f, "count", 1);
				op.stride = Get(f, "stride", 4);
				op.value = Get(f, "value", 0);
			}
			else if (Has(n, "copy32"))
			{
				const auto c = Child(n, "copy32");
				op.kind = StepKind::Copy32;
				op.from = Get(c, "from", 0);
				op.to = Get(c, "to", 0);
			}
			else if (Has(n, "age"))
			{
				const auto a = Child(n, "age");
				op.kind = StepKind::Age;
				op.addr = Get(a, "table", 0);
				op.count = Get(a, "count", 0);
				op.stride = Get(a, "stride", 8);
				op.id_off = Get(a, "id_off", 0);
				op.age_off = Get(a, "age_off", 4);
				op.limit = Get(a, "limit", 0);
				op.if_nonzero = Get(a, "if_nonzero", 0);
			}
			return op;
		}
		std::vector<u32> ParseList(const ryml::ConstNodeRef& n)
		{
			std::vector<u32> out;
			for (const ryml::ConstNodeRef& c : n.children())
				out.push_back(c.is_map() ? Get(c, "addr", 0) : ParseU32(c));
			return out;
		}

		bool ParseManifest(const std::string& text, const std::string& path, Manifest* m, std::string* error)
		{
			Error err;
			std::optional<ryml::Tree> tree = ParseYAMLFromString(ryml::to_csubstr(text), ryml::to_csubstr(path), &err);
			if (!tree.has_value())
			{
				if (error)
					*error = fmt::format("YAML: {}", err.GetDescription());
				return false;
			}
			const ryml::ConstNodeRef root = tree->crootref();
			if (Has(root, "game"))
			{
				m->serial = GetStr(Child(root, "game"), "serial");
				m->name = GetStr(Child(root, "game"), "name");
			}
			if (Has(root, "state"))
			{
				const auto st = Child(root, "state");
				if (Has(st, "regions"))
				{
					for (const auto& c : Child(st, "regions").children())
					{
						RegionSpec r;
						if (c.is_seq())
						{
							r.addr = ParseU32(c[0]);
							r.len = ParseVal(c[1]);
						}
						else
						{
							r.addr = Get(c, "addr", 0);
							if (Has(c, "len"))
								r.len = ParseVal(Child(c, "len"));
							if (Has(c, "end"))
							{
								r.has_end = true;
								r.end = ParseVal(Child(c, "end"));
							}
						}
						m->regions.push_back(r);
					}
				}
				// generated lists kept next to the manifest: "ADDR_HEX LEN_HEX [anything]" per line, # comments
				auto list_file = [&](const char* key, auto&& add) {
					if (!Has(st, key))
						return;
					const std::string rel = GetStr(st, key);
					const std::string file = Path::Combine(Path::GetDirectory(path), rel);
					std::optional<std::string> text = FileSystem::ReadFileToString(file.c_str());
					if (!text.has_value())
					{
						Console.Error("GameRollback: cannot read %s", file.c_str());
						return;
					}
					size_t pos = 0;
					while (pos < text->size())
					{
						size_t nl = text->find('\n', pos);
						if (nl == std::string::npos)
							nl = text->size();
						const std::string line = text->substr(pos, nl - pos);
						pos = nl + 1;
						if (line.empty() || line[0] == '#')
							continue;
						char* e1 = nullptr;
						const u32 a = static_cast<u32>(std::strtoul(line.c_str(), &e1, 16));
						if (e1 == line.c_str())
							continue;
						const u32 l = static_cast<u32>(std::strtoul(e1, nullptr, 16));
						if (l)
							add(a, l);
					}
				};
				list_file("exclude_file", [&](u32 a, u32 l) { m->excludes.push_back({{{a}}, l, {}}); });
				list_file("ignore_file", [&](u32 a, u32 l) { m->ignores.push_back({{{a}}, l, {}}); });
				list_file("resim_restore_file", [&](u32 a, u32 l) { m->resim_restore.push_back({a, l}); });
				if (Has(st, "exclude"))
					for (const auto& c : Child(st, "exclude").children())
						m->excludes.push_back(ParseItem(c));
				if (Has(st, "ignore"))
					for (const auto& c : Child(st, "ignore").children())
						m->ignores.push_back(ParseItem(c));
				if (Has(st, "watch"))
					for (const auto& c : Child(st, "watch").children())
						m->watches.push_back(ParseItem(c));
				if (Has(st, "exclude_thread_stacks"))
					m->exclude_thread_stacks = GetStr(st, "exclude_thread_stacks") != "false";
				if (Has(st, "gate_stable"))
					m->gate_stable = ParseRanges(Child(st, "gate_stable"));
				if (Has(st, "resim_restore"))
					m->resim_restore = ParseRanges(Child(st, "resim_restore"));
				if (Has(st, "dynamic"))
				{
					for (const auto& c : Child(st, "dynamic").children())
					{
						Dynamic d;
						d.kind = GetStr(c, "kind") == "exclude_resim_restore" ? DynKind::ExcludeResimRestore : DynKind::Exclude;
						if (Has(c, "table"))
							d.table = ParseTable(Child(c, "table"));
						if (Has(c, "list"))
						{
							const auto l = Child(c, "list");
							ListSpec ls;
							ls.head = Get(l, "head", 0);
							ls.next_off = Get(l, "next_off", 0);
							ls.max = Get(l, "max", 64);
							if (Has(c, "entries"))
							{
								const auto e = Child(c, "entries");
								ls.first = Get(e, "first", 0);
								ls.stride = Get(e, "stride", 0);
								ls.count = Get(e, "count", 0);
								ls.extra_ptr = Get(e, "extra_ptr", 0);
							}
							d.list = ls;
						}
						if (Has(c, "chain"))
							d.chain = ParseChain(Child(c, "chain"));
						if (Has(c, "expr"))
						{
							d.expr = ParseVal(Child(c, "expr"));
							if (Has(c, "count"))
								d.count = ParseVal(Child(c, "count"));
							if (Has(c, "len"))
								d.len_val = ParseVal(Child(c, "len"));
						}
						if (Has(c, "fields"))
							d.fields = ParseRanges(Child(c, "fields"));
						if (Has(c, "lens"))
							d.lens = ParseList(Child(c, "lens"));
						d.len = Get(c, "len", 0);
						m->dynamic.push_back(std::move(d));
					}
				}
			}
			if (Has(root, "macros"))
				for (const auto& c : Child(root, "macros").children())
					m->macros[std::string(c.key().str, c.key().len)] = std::string(c.val().str, c.val().len);
			if (Has(root, "rollback"))
			{
				const auto rb = Child(root, "rollback");
				if (Has(rb, "gate_when"))
					m->gate_when = ParseVal(Child(rb, "gate_when"));
				m->gate_counter = Get(rb, "gate_counter", 0);
				if (Has(rb, "input_block"))
					m->input_block = ParseRange(Child(rb, "input_block"));
				if (Has(rb, "rng_split"))
					m->rng_split = ParseRange(Child(rb, "rng_split"));
			}
			if (Has(root, "frame"))
			{
				const auto fr = Child(root, "frame");
				m->sim_tick_site = Get(fr, "sim_tick_site", 0);
				m->return_addr = Get(fr, "return_addr", 0);
				m->render_begin_site = Get(fr, "render_begin_site", 0);
				m->render_end_site = Get(fr, "render_end_site", 0);
			}
			if (Has(root, "resim_steps"))
			{
				for (const auto& c : Child(root, "resim_steps").children())
				{
					Step s;
					if (Has(c, "call"))
					{
						s.kind = StepKind::Call;
						s.fn = Get(c, "call", 0);
						s.has_a0 = Has(c, "a0");
						s.a0 = Get(c, "a0", 0);
					}
					else if (Has(c, "call_each"))
					{
						const auto e = Child(c, "call_each");
						s.kind = StepKind::CallEach;
						s.fn = Get(e, "fn", 0);
						s.each = ParseTable(Child(e, "table"));
					}
					else
					{
						s.op = ParseOp(c);
						s.kind = s.op.kind;
					}
					m->steps.push_back(s);
				}
			}
			if (Has(root, "hooks"))
			{
				const auto h = Child(root, "hooks");
				if (Has(h, "resim_gate"))
				{
					for (const auto& c : Child(h, "resim_gate").children())
					{
						const u32 a = c.is_map() ? Get(c, "addr", 0) : ParseU32(c);
						m->resim_gates.push_back(a);
						if (c.is_map() && Has(c, "v0"))
							m->gate_values[a] = Get(c, "v0", 0);
					}
				}
				if (Has(h, "resim_skip_call"))
					m->resim_skips = ParseList(Child(h, "resim_skip_call"));
				if (Has(h, "skip_call"))
					m->always_skips = ParseList(Child(h, "skip_call"));
				if (Has(h, "entry_actions"))
				{
					for (const auto& c : Child(h, "entry_actions").children())
					{
						EntryAction a;
						a.at = Get(c, "at", 0);
						a.resim_only = GetStr(c, "when") != "always";
						a.ret = GetStr(c, "then") != "continue";
						if (Has(c, "ops"))
							for (const auto& o : Child(c, "ops").children())
								a.ops.push_back(ParseOp(o));
						m->entry_actions.push_back(std::move(a));
					}
				}
			}
			if (Has(root, "virtual_streams"))
			{
				const auto v = Child(root, "virtual_streams");
				VirtualStreams& vs = m->vs;
				vs.state = Get(v, "state", 0);
				vs.channels = std::min<u32>(Get(v, "channels", 4), 8);
				vs.k_prep = Get(v, "k_prep", 13);
				vs.ready_addr = Get(v, "ready", 0);
				auto reg = [](const std::string& n) -> int {
					static const char* names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4",
						"t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp",
						"ra"};
					for (int k = 0; k < 32; k++)
						if (n == names[k])
							return k;
					if (n.size() > 1 && n[0] == 'f')
						return 100 + std::atoi(n.c_str() + 1);
					return 4;
				};
				if (Has(v, "lengths_file"))
				{
					const std::string file = Path::Combine(Path::GetDirectory(path), GetStr(v, "lengths_file"));
					std::optional<std::string> text = FileSystem::ReadFileToString(file.c_str());
					if (!text.has_value())
						Console.Error("GameRollback: cannot read %s", file.c_str());
					else
					{
						size_t pos = text->find('\n') + 1; // header
						while (pos > 0 && pos < text->size())
						{
							size_t nl = text->find('\n', pos);
							if (nl == std::string::npos)
								nl = text->size();
							std::vector<std::string> col;
							std::string cur;
							for (size_t k = pos; k < nl; k++)
							{
								if (text->at(k) == ',')
								{
									col.push_back(cur);
									cur.clear();
								}
								else if (text->at(k) != '\r')
									cur += text->at(k);
							}
							col.push_back(cur);
							pos = nl + 1;
							if (col.size() < 15)
								continue;
							VsLen l;
							l.frames = static_cast<u32>(std::strtoul(col[8].c_str(), nullptr, 10));
							l.loops = col[10] == "1";
							l.loop_start = static_cast<u32>(std::strtoul(col[13].c_str(), nullptr, 10));
							l.loop_end = static_cast<u32>(std::strtoul(col[14].c_str(), nullptr, 10));
							const u64 key = (static_cast<u64>(std::strtoul(col[0].c_str(), nullptr, 10)) << 32) |
											std::strtoul(col[1].c_str(), nullptr, 10);
							vs.lengths[key] = l;
						}
					}
				}
				if (Has(v, "hooks"))
				{
					for (const auto& c : Child(v, "hooks").children())
					{
						VsHook h;
						h.kind = GetStr(c, "kind");
						h.at = Get(c, "at", 0);
						if (Has(c, "ra"))
							for (const auto& r : Child(c, "ra").children())
							{
								const std::string t(r.val().str, r.val().len);
								h.ra.push_back(t == "return" ? m->return_addr : ParseU32(r));
							}
						if (Has(c, "ch"))
							h.ch_reg = reg(GetStr(c, "ch"));
						if (Has(c, "idx"))
							h.idx_reg = reg(GetStr(c, "idx"));
						if (Has(c, "prio"))
							h.prio_reg = reg(GetStr(c, "prio"));
						if (Has(c, "paused"))
							h.paused_reg = reg(GetStr(c, "paused"));
						if (Has(c, "dur"))
							h.dur_reg = reg(GetStr(c, "dur"));
						if (Has(c, "target"))
							h.target_reg = reg(GetStr(c, "target"));
						h.ch_mask = Get(c, "ch_mask", 0xFFFFFFFFu);
						h.need_ready = GetStr(c, "ready") == "true";
						h.value = GetStr(c, "value");
						h.not_ready = static_cast<s32>(Get(c, "not_ready", 0));
						vs.hooks.push_back(std::move(h));
					}
				}
			}
			if (Has(root, "pad"))
			{
				m->pad_read_fn = Get(Child(root, "pad"), "read_fn", 0);
				m->pad_site = Get(Child(root, "pad"), "site", 0);
				m->pad_mode = Get(Child(root, "pad"), "mode", 0x73);
				m->pad_record_replay = GetStr(Child(root, "pad"), "record_replay") == "true";
				if (Has(Child(root, "pad"), "replay"))
				{
					const auto rp = Child(Child(root, "pad"), "replay");
					auto reg = [](const std::string& n) -> int {
						static const char* names[8] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
						for (int k = 0; k < 8; k++)
							if (n == names[k])
								return 4 + k;
						return 4;
					};
					m->replay_fn = Get(rp, "fn", 0);
					m->replay_site = Get(rp, "site", 0);
					m->replay_len = std::min<u32>(Get(rp, "len", 32), 32);
					m->replay_buf_reg = reg(GetStr(rp, "buf"));
					if (Has(rp, "player"))
						for (const auto& c : Child(rp, "player").children())
							m->replay_player_regs.push_back(reg(std::string(c.val().str, c.val().len)));
					m->pad_record_replay = true;
				}
			}
			if (Has(root, "rng"))
			{
				const auto r = Child(root, "rng");
				m->rng_float = Get(r, "range_float", 0);
				m->rng_int = Get(r, "range_int", 0);
				m->sound_seed = Get(r, "sound_seed", 0);
				if (Has(r, "sound_sites"))
					for (const u32 s : ParseList(Child(r, "sound_sites")))
						m->sound_sites.insert(s);
				if (Has(r, "trace"))
					for (const auto& c : Child(r, "trace").children())
						m->trace_ids[Get(c, "fn", 0)] = Get(c, "id", 0);
			}
			if (!m->sim_tick_site || !m->return_addr || m->steps.empty())
			{
				if (error)
					*error = "manifest needs frame.sim_tick_site, frame.return_addr and resim_steps";
				return false;
			}
			return true;
		}

		// ---------------------------------------------------------------------------------------------------------
		// EE memory helpers
		// ---------------------------------------------------------------------------------------------------------
		u32 Rd(u32 a)
		{
			u32 v;
			std::memcpy(&v, &eeMem->Main[a & RAM_MASK & ~3u], 4);
			return v;
		}
		u32 Rd16(u32 a)
		{
			u16 v;
			std::memcpy(&v, &eeMem->Main[a & RAM_MASK & ~1u], 2);
			return v;
		}
		u32 Rd8(u32 a) { return eeMem->Main[a & RAM_MASK]; }
		void Wr(u32 a, u32 v) { std::memcpy(&eeMem->Main[a & RAM_MASK & ~3u], &v, 4); }
		bool PtrOk(u32 p) { return p >= 0x00100000u && p < 0x02000000u; }
		std::optional<u32> Resolve(const AddrSpec& s)
		{
			if (s.chain.empty())
				return std::nullopt;
			if (s.chain.size() == 1)
				return s.chain[0];
			u32 p = Rd(s.chain[0]);
			for (size_t k = 1; k + 1 < s.chain.size(); k++)
			{
				if (!PtrOk(p))
					return std::nullopt;
				p = Rd(p + s.chain[k]);
			}
			if (!PtrOk(p))
				return std::nullopt;
			return p + s.chain.back();
		}
		void TablePtrs(const TableSpec& t, std::vector<u32>* out)
		{
			for (u32 i = 0; i < t.count; i++)
			{
				const u32 e = t.base + i * t.stride;
				if (t.used_off != NONE && Rd(e + t.used_off) == 0)
				{
					out->push_back(0);
					continue;
				}
				const u32 p = Rd(e + t.ptr_off);
				out->push_back(PtrOk(p) ? p : 0);
			}
		}
		void DoOp(const Op& op)
		{
			switch (op.kind)
			{
				case StepKind::Fill32:
					for (u32 i = 0; i < op.count; i++)
						Wr(op.addr + i * op.stride, op.value);
					break;
				case StepKind::Copy32:
					Wr(op.to, Rd(op.from));
					break;
				case StepKind::Age:
					if (op.if_nonzero && Rd(op.if_nonzero) == 0)
						break;
					for (u32 i = 0; i < op.count; i++)
					{
						const u32 e = op.addr + i * op.stride;
						if (static_cast<s32>(Rd(e + op.id_off)) <= 0)
							continue;
						const u32 age = Rd(e + op.age_off) + 1;
						Wr(e + op.age_off, age);
						if (static_cast<s32>(age) >= static_cast<s32>(op.limit))
							Wr(e + op.id_off, 0);
					}
					break;
				default:
					break;
			}
		}

		// ---------------------------------------------------------------------------------------------------------
		// state (EE thread unless noted)
		// ---------------------------------------------------------------------------------------------------------
		std::mutex s_req_mtx; // guards the pending request fields (any thread -> EE thread)
		struct Request
		{
			bool attach = false, detach = false, start = false, stop = false, net_start = false, net_stop = false;
			NetBridge::Config net;
			std::string path;
			int mode = 0;
			u32 frames = 8;
		} s_req;
		bool s_req_pending = false;

		Manifest s_man;
		std::string s_path;
		bool s_attached = false;
		int s_mode = 0;
		u32 s_frames = 8;
		std::string s_status = "gamerb: detached";

		// re-simulation driver
		bool s_driving = false, s_passthrough = false;
		u32 s_R = 0, s_i = 0;
		size_t s_step = 0;
		std::vector<u32> s_each;
		size_t s_each_i = 0;
		bool s_each_built = false;

		// pad feed
		bool s_pad_pending = false;
		u32 s_pad_player = 0, s_pad_buf = 0;
		// pad record/replay (games whose re-simulated frame reads the pad again): the raw report of every player on
		// every real frame, replayed into the same read when that frame is re-simulated
		constexpr u32 PAD_HIST = 64, PAD_PLAYERS = 8, PAD_REPORT = 32;
		struct PadRec
		{
			s32 frame = -1;
			u32 ret = 0;
			u8 report[PAD_REPORT] = {};
		};
		PadRec s_pad_hist[PAD_HIST][PAD_PLAYERS];
		s32 s_host_frame = -1; // frame index of the current real frame (advanced at every FRAME_BEGIN)

		// netplay (NetBridge): every player's report is rebuilt from the 6-byte wire input, identically on every peer
		bool s_net = false;
		NetBridge::Plan s_net_plan;
		std::vector<s32> s_net_resim_saves; // save index per re-simulated step
		s32 s_net_fwd_save = -1;            // the forward frame's save, answered at the next frame boundary
		u8 s_live_report[PAD_PLAYERS][PAD_REPORT] = {};
		bool s_live_valid[PAD_PLAYERS] = {};
		std::vector<std::pair<u32, u32>> s_hash_ranges; // the manifest's watched (gameplay) state
		u64 s_net_refused = 0;

		u32 HashState()
		{
			u64 h = 1469598103934665603ull;
			for (const auto& [a, l] : s_hash_ranges)
			{
				const u8* p = &eeMem->Main[a & RAM_MASK];
				for (u32 k = 0; k < l; k++)
					h = (h ^ p[k]) * 1099511628211ull;
			}
			return static_cast<u32>(h ^ (h >> 32));
		}
		void BuildReport(const u8* in, u8* out)
		{
			std::memset(out, 0, PAD_REPORT);
			out[0] = 0;
			out[1] = static_cast<u8>(s_man.pad_mode);
			out[2] = static_cast<u8>(~in[0]);
			out[3] = static_cast<u8>(~in[1]);
			out[4] = in[2];
			out[5] = in[3];
			out[6] = in[4];
			out[7] = in[5];
			if ((s_man.pad_mode & 0x0F) >= 9)
			{
				const u16 buttons = static_cast<u16>((in[0] << 8) | in[1]); // PadFeed layout ((b2 << 8) | b3) ^ 0xFFFF
				static constexpr u16 PRESS_BITS[12] = {0x2000, 0x8000, 0x1000, 0x4000, 0x10, 0x20, 0x40, 0x80, 0x4, 0x8, 0x1, 0x2};
				for (u32 i = 0; i < 12; i++)
					out[8 + i] = (buttons & PRESS_BITS[i]) ? 0xFF : 0x00;
			}
		}
		void ReportToInput(const u8* rep, u8* out)
		{
			out[0] = static_cast<u8>(~rep[2]);
			out[1] = static_cast<u8>(~rep[3]);
			out[2] = rep[4];
			out[3] = rep[5];
			out[4] = rep[6];
			out[5] = rep[7];
		}

		// dynamic ranges
		u64 s_dyn_sig = 0;

		// watchdog: a thread that reports where the EE is when frame boundaries stop arriving while rollback is on
		std::atomic<u64> s_heartbeat{0};
		std::atomic<bool> s_watchdog_run{false};
		std::thread s_watchdog;
		std::string DumpEeThreads();
		void WatchdogLoop()
		{
			u64 last = s_heartbeat.load();
			int stale = 0;
			bool reported = false;
			while (s_watchdog_run.load())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				const u64 hb = s_heartbeat.load();
				if (hb != last || !s_attached)
				{
					last = hb;
					stale = 0;
					reported = false;
					continue;
				}
				if (++stale >= 6 && !reported) // 3 s without a frame boundary
				{
					reported = true;
					Console.Error("GameRollback watchdog: no frame boundary for 3 s: EE pc %08X ra %08X sp %08X epc %08X errorepc %08X "
								  "| driving %d step %zu i %u/%u passthrough %d mode %d resim %d",
						cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0], cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.ErrorEPC,
						s_driving, s_step, s_i, s_R, s_passthrough, s_mode, RollbackDevice::IsResimulating());
					// sample the pc a few times: a busy-wait shows the same few user pcs (EPC) around its syscalls
					for (int k = 0; k < 8; k++)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(37));
						Console.Error("  sample pc %08X epc %08X ra %08X v1 %08X", cpuRegs.pc, cpuRegs.CP0.n.EPC,
							cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v1.UL[0]);
					}
					Console.Error("GameRollback watchdog: EE threads:%s", DumpEeThreads().c_str());
					if (RollbackDevice::TraceOn()) // what the EE called last (probe log), next to the manifest
					{
						const std::string out = Path::Combine(Path::GetDirectory(s_path), "watchdog_probe_log.txt");
						RollbackDevice::ProbeLogDump(out);
						Console.Error("GameRollback watchdog: probe log -> %s", out.c_str());
					}
				}
			}
		}

		// hot reload
		bool s_watch = true;
		s64 s_mtime = 0;
		u32 s_watch_tick = 0;

		// PCSX2 finds the kernel's EE thread table lazily at the first StartThread syscall after a BIOS boot; a session
		// started from a savestate never sees one. Same pattern scan here (kernel memory is physical 0x0..0x5000).
		void EnsureEeThreadList()
		{
			if (CurrentBiosInformation.eeThreadListAddr != 0)
				return;
			for (u32 off = 0; off < 0x5000; off += 4)
			{
				if (Rd(off) == ThreadListInstructions[0] && Rd(off + 4) == ThreadListInstructions[1] &&
					Rd(off + 8) == ThreadListInstructions[2])
				{
					CurrentBiosInformation.eeThreadListAddr = 0x80010000 + static_cast<u16>(Rd(off + 24)) - 8;
					Console.WriteLn("GameRollback: EE thread table at %08X", CurrentBiosInformation.eeThreadListAddr);
					return;
				}
			}
		}
		std::string DumpEeThreads()
		{
			std::string out;
			if (CurrentBiosInformation.eeThreadListAddr == 0 || CurrentBiosInformation.eeThreadListAddr == 0xFFFFFFFFu)
				return "(no thread table)";
			const u32 start = CurrentBiosInformation.eeThreadListAddr & 0x3fffff;
			for (u32 tid = 0; tid < 256; tid++)
			{
				const EEInternalThread* t = static_cast<const EEInternalThread*>(PSM(start + tid * sizeof(EEInternalThread)));
				if (!t || t->status == static_cast<int>(ThreadStatus::THS_BAD))
					continue;
				out += fmt::format("\n  tid {:3} status {:#04x} wait {} sema {} prio {} entry {:08X} pc {:08X} stack {:08X}+{:X}", tid,
					t->status, t->waitType, t->semaId, t->currentPriority, t->entry, t->resumeAddr, t->stackMem, t->stackSize);
			}
			return out;
		}

		// ---------------------------------------------------------------------------------------------------------
		// dynamic ranges (pointer walks), applied at the frame boundary when the objects moved
		// ---------------------------------------------------------------------------------------------------------
		void RefreshDynamic(bool force)
		{
			std::vector<std::pair<u32, u32>> ex, rr;
			u64 sig = 1469598103934665603ull;
			auto mix = [&sig](u32 v) { sig = (sig ^ v) * 1099511628211ull; };
			for (const Dynamic& d : s_man.dynamic)
			{
				std::vector<std::pair<u32, u32>> out;
				if (d.table)
				{
					std::vector<u32> ptrs;
					TablePtrs(*d.table, &ptrs);
					for (size_t i = 0; i < ptrs.size(); i++)
					{
						mix(ptrs[i]);
						if (!ptrs[i])
							continue;
						for (size_t f = 0; f < d.fields.size(); f++)
						{
							u32 len = d.fields[f].len;
							if (f == 0 && i < d.lens.size())
								len = d.lens[i];
							out.emplace_back(ptrs[i] + d.fields[f].addr, len);
						}
					}
				}
				else if (d.list)
				{
					const ListSpec& l = *d.list;
					const u32 extra = l.extra_ptr ? Rd(l.extra_ptr) : 0;
					mix(extra);
					u32 cur = Rd(l.head);
					for (u32 n = 0; n < l.max && cur != l.head && PtrOk(cur); n++)
					{
						mix(cur);
						for (u32 e = 0; e < l.count; e++)
						{
							const u32 base = cur + l.first + e * l.stride + extra;
							for (const Range& f : d.fields)
								out.emplace_back(base + f.addr, f.len);
						}
						cur = Rd(cur + l.next_off);
					}
				}
				else if (d.expr)
				{
					s64 n = 0;
					if (Eval(d.count, 0, &n))
					{
						for (s64 i = 0; i < n && i < 4096; i++)
						{
							s64 a = 0, l = 0;
							if (!Eval(*d.expr, i, &a) || !Eval(d.len_val, i, &l) || a <= 0 || l <= 0 || a >= 0x02000000)
								continue;
							mix(static_cast<u32>(a));
							mix(static_cast<u32>(l));
							out.emplace_back(static_cast<u32>(a), static_cast<u32>(l));
						}
					}
				}
				else if (d.chain)
				{
					if (const std::optional<u32> a = Resolve(*d.chain))
					{
						mix(*a);
						out.emplace_back(*a, d.len);
					}
				}
				ex.insert(ex.end(), out.begin(), out.end());
				if (d.kind == DynKind::ExcludeResimRestore)
					rr.insert(rr.end(), out.begin(), out.end());
			}
			// Every EE thread's stack (from the kernel's thread table): a thread's saved context lives in kernel memory,
			// which is never rolled back, so its stack must not be either (else the thread resumes on a rewound stack).
			EnsureEeThreadList();
			if (s_man.exclude_thread_stacks && CurrentBiosInformation.eeThreadListAddr &&
				CurrentBiosInformation.eeThreadListAddr != 0xFFFFFFFFu)
			{
				const u32 start = CurrentBiosInformation.eeThreadListAddr & 0x3fffff;
				for (u32 tid = 0; tid < 256; tid++)
				{
					const EEInternalThread* t = static_cast<const EEInternalThread*>(PSM(start + tid * sizeof(EEInternalThread)));
					if (!t || t->status == static_cast<int>(ThreadStatus::THS_BAD) || !t->stackMem || t->stackSize <= 0)
						continue;
					mix(t->stackMem);
					mix(static_cast<u32>(t->stackSize));
					ex.emplace_back(t->stackMem & RAM_MASK, static_cast<u32>(t->stackSize));
				}
			}
			if (!force && sig == s_dyn_sig)
				return;
			s_dyn_sig = sig;
			RollbackDevice::SetDynamicExcludes(ex);
			RollbackDevice::SetDynamicResimRestore(rr);
		}

		// ---------------------------------------------------------------------------------------------------------
		// hook handlers
		// ---------------------------------------------------------------------------------------------------------
		void SetCall(u32 fn, bool has_a0, u32 a0)
		{
			cpuRegs.GPR.n.ra.UD[0] = s_man.return_addr;
			if (has_a0)
				cpuRegs.GPR.n.a0.SD[0] = static_cast<s32>(a0);
			cpuRegs.pc = fn;
		}
		void BeginResimFrame()
		{
			RollbackDevice::HandleSyscall(RollbackDevice::CMD_RESIM_PRE, s_i, 0, 0);
			s_step = 0;
			s_each_built = false;
		}
		// Runs host-side steps until the next game call (pc set: Jump) or the end of the whole re-simulation.
		EeHooks::Action RunSteps()
		{
			for (;;)
			{
				while (s_step < s_man.steps.size())
				{
					const Step& st = s_man.steps[s_step];
					switch (st.kind)
					{
						case StepKind::Call:
							SetCall(st.fn, st.has_a0, st.a0);
							s_step++;
							return EeHooks::Action::Jump;
						case StepKind::CallEach:
							if (!s_each_built)
							{
								s_each.clear();
								TablePtrs(st.each, &s_each);
								s_each_i = 0;
								s_each_built = true;
							}
							while (s_each_i < s_each.size() && !s_each[s_each_i])
								s_each_i++;
							if (s_each_i < s_each.size())
							{
								SetCall(st.fn, true, s_each[s_each_i++]);
								return EeHooks::Action::Jump;
							}
							s_each_built = false;
							s_step++;
							break;
						default:
							DoOp(st.op);
							s_step++;
							break;
					}
				}
				RollbackDevice::HandleSyscall(RollbackDevice::CMD_RESIM_POST, s_i, 0, 0);
				if (s_net && s_i < s_net_resim_saves.size())
					NetBridge::ResolveSave(s_net_resim_saves[s_i], HashState());
				if (++s_i < s_R)
				{
					BeginResimFrame();
					continue;
				}
				RollbackDevice::HandleSyscall(RollbackDevice::CMD_CUR_PRE, 0, 0, 0);
				s_driving = false;
				s_passthrough = true;
				cpuRegs.pc = s_man.sim_tick_site; // the original call now runs for the current frame
				return EeHooks::Action::Jump;
			}
		}

		void ApplyRequests();

		// Netplay, before FRAME_BEGIN: answer the previous forward frame's save, get this frame's plan (holding while
		// the netcode waits), write every planned frame's inputs as rebuilt reports, request the rollback depth.
		bool NetFrameBegin()
		{
			if (s_net_fwd_save >= 0)
			{
				NetBridge::ResolveSave(s_net_fwd_save, HashState());
				s_net_fwd_save = -1;
			}
			int n = 0;
			for (int waited = 0;; waited++)
			{
				n = NetBridge::Frame(&s_net_plan);
				if (n != 0)
					break;
				if (waited > 10000) // ~10 s without a frame: give up
				{
					n = -1;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (n < 0)
			{
				Console.Error("GameRollback: netcode stopped (%s)", NetBridge::Status().c_str());
				NetBridge::Stop();
				s_net = false;
				return false;
			}
			s_net_resim_saves.clear();
			for (const NetBridge::Step& st : s_net_plan.steps)
			{
				for (u32 p = 0; p < 2; p++)
				{
					PadRec& r = s_pad_hist[static_cast<u32>(st.frame) % PAD_HIST][p];
					r.frame = st.frame;
					r.ret = 1;
					BuildReport(st.inputs[p], r.report);
				}
				if (st.rolling_back)
					s_net_resim_saves.push_back(st.save_index);
				else
				{
					s_net_fwd_save = st.save_index;
					if (st.frame != s_host_frame + 1)
						Console.Error("GameRollback: netcode frame %d != host frame %d", st.frame, s_host_frame + 1);
				}
			}
			RollbackDevice::SetExternalRollback(s_net_plan.rollback_advances);
			return true;
		}

		EeHooks::Action OnSimTickSite(u32)
		{
			s_heartbeat.fetch_add(1, std::memory_order_relaxed);
			if (s_passthrough)
			{
				s_passthrough = false;
				return EeHooks::Action::Continue;
			}
			ApplyRequests();
			if (s_mode == 0)
				return EeHooks::Action::Continue;
			RefreshDynamic(false);
			if (s_man.gate_when)
			{
				s64 v = 0;
				RollbackDevice::SetFrameGateCondition(Eval(*s_man.gate_when, 0, &v) && v != 0);
			}
			if (s_net && !NetFrameBegin())
				return EeHooks::Action::Continue; // session failed: the frame runs without netcode
			s_host_frame++;
			const u32 R = static_cast<u32>(RollbackDevice::HandleSyscall(RollbackDevice::CMD_FRAME_BEGIN, 0, 0, 0));
			if (s_net && R != s_net_plan.rollback_advances)
			{
				s_net_refused++;
				Console.Error("GameRollback: netcode asked for a %u-frame rollback at frame %d, device did %u (gated)",
					s_net_plan.rollback_advances, s_host_frame, R);
			}
			if (R == 0)
			{
				RollbackDevice::HandleSyscall(RollbackDevice::CMD_CUR_PRE, 0, 0, 0);
				return EeHooks::Action::Continue;
			}
			s_R = R;
			s_i = 0;
			s_driving = true;
			BeginResimFrame();
			return RunSteps();
		}

		EeHooks::Action OnReturnAddr(u32)
		{
			s_heartbeat.fetch_add(1, std::memory_order_relaxed);
			if (!s_driving)
			{
				Console.Error("GameRollback: return address reached outside re-simulation (pc %08X ra %08X)", cpuRegs.pc,
					cpuRegs.GPR.n.ra.UL[0]);
				return EeHooks::Action::Continue;
			}
			return RunSteps();
		}

		EeHooks::Action OnPadRead(u32)
		{
			// netplay always replays at the feed point: resim frames need the corrected raw reports the netcode wrote
			if (cpuRegs.GPR.n.ra.UL[0] == s_man.pad_site + 8 && (s_net || (s_man.pad_record_replay && !s_man.replay_fn)) && s_driving)
			{
				// re-simulated frame: answer the read from the recorded report without running the pad library
				// (its IOP-side state is live hardware; the report is all the game gets from it)
				const u32 player = (cpuRegs.GPR.n.a0.UL[0] + cpuRegs.GPR.n.a1.UL[0]) % PAD_PLAYERS;
				const s32 f = s_host_frame - static_cast<s32>(s_R) + static_cast<s32>(s_i);
				const PadRec& r = s_pad_hist[static_cast<u32>(f) % PAD_HIST][player];
				if (f >= 0 && r.frame == f)
				{
					std::memcpy(&eeMem->Main[cpuRegs.GPR.n.a2.UL[0] & RAM_MASK], r.report, PAD_REPORT);
					cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(r.ret);
					return EeHooks::Action::Return;
				}
			}
			if (cpuRegs.GPR.n.ra.UL[0] == s_man.pad_site + 8)
			{
				s_pad_player = cpuRegs.GPR.n.a0.UL[0] + cpuRegs.GPR.n.a1.UL[0];
				s_pad_buf = cpuRegs.GPR.n.a2.UL[0];
				s_pad_pending = true;
			}
			return EeHooks::Action::Continue;
		}
		EeHooks::Action OnPadReadReturn(u32)
		{
			if (!s_pad_pending)
				return EeHooks::Action::Continue;
			s_pad_pending = false;
			const u32 player = s_pad_player % PAD_PLAYERS;
			u8* buf = &eeMem->Main[s_pad_buf & RAM_MASK];
			if ((s_net || (s_man.pad_record_replay && !s_man.replay_fn)) && s_driving)
			{
				// re-simulated frame: the report this player's read returned when the frame ran for real
				const s32 f = s_host_frame - static_cast<s32>(s_R) + static_cast<s32>(s_i);
				const PadRec& r = s_pad_hist[static_cast<u32>(f) % PAD_HIST][player];
				if (f >= 0 && r.frame == f)
				{
					std::memcpy(buf, r.report, PAD_REPORT);
					cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(r.ret);
				}
				return EeHooks::Action::Continue;
			}
			cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(RollbackDevice::HandleSyscall(
				RollbackDevice::CMD_PAD_FEED, s_pad_player, s_pad_buf, cpuRegs.GPR.n.v0.UL[0]));
			if (s_net)
			{
				// this peer's live input (real pad or feed) goes to the netcode; the game gets the session's input
				std::memcpy(s_live_report[player], buf, PAD_REPORT);
				s_live_valid[player] = true;
				const PadRec& r = s_pad_hist[static_cast<u32>(s_host_frame) % PAD_HIST][player];
				if (r.frame == s_host_frame)
				{
					std::memcpy(buf, r.report, PAD_REPORT);
					cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(r.ret);
				}
				return EeHooks::Action::Continue;
			}
			if (s_man.pad_record_replay && !s_man.replay_fn && s_mode != 0 && s_host_frame >= 0)
			{
				PadRec& r = s_pad_hist[static_cast<u32>(s_host_frame) % PAD_HIST][player];
				r.frame = s_host_frame;
				r.ret = cpuRegs.GPR.n.v0.UL[0];
				std::memcpy(r.report, buf, PAD_REPORT);
			}
			return EeHooks::Action::Continue;
		}

		// Separate replay point (pad.replay): resim frames get the recorded output without running the wrapped read
		bool s_replay_pending = false;
		u32 s_replay_player = 0, s_replay_buf = 0;
		u32 ReplayPlayer()
		{
			u32 p = 0;
			for (const int r : s_man.replay_player_regs)
				p += cpuRegs.GPR.r[r].UL[0];
			return p % PAD_PLAYERS;
		}
		EeHooks::Action OnReplayEntry(u32)
		{
			if (s_net || cpuRegs.GPR.n.ra.UL[0] != s_man.replay_site + 8)
				return EeHooks::Action::Continue;
			const u32 player = ReplayPlayer();
			const u32 buf = cpuRegs.GPR.r[s_man.replay_buf_reg].UL[0];
			if (s_driving)
			{
				const s32 f = s_host_frame - static_cast<s32>(s_R) + static_cast<s32>(s_i);
				const PadRec& r = s_pad_hist[static_cast<u32>(f) % PAD_HIST][player];
				if (f >= 0 && r.frame == f)
				{
					std::memcpy(&eeMem->Main[buf & RAM_MASK], r.report, s_man.replay_len);
					cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(r.ret);
					return EeHooks::Action::Return;
				}
				return EeHooks::Action::Continue;
			}
			s_replay_player = player;
			s_replay_buf = buf;
			s_replay_pending = true;
			return EeHooks::Action::Continue;
		}
		EeHooks::Action OnReplayReturn(u32)
		{
			if (!s_replay_pending)
				return EeHooks::Action::Continue;
			s_replay_pending = false;
			if (s_mode != 0 && s_host_frame >= 0)
			{
				PadRec& r = s_pad_hist[static_cast<u32>(s_host_frame) % PAD_HIST][s_replay_player];
				r.frame = s_host_frame;
				r.ret = cpuRegs.GPR.n.v0.UL[0];
				std::memcpy(r.report, &eeMem->Main[s_replay_buf & RAM_MASK], s_man.replay_len);
			}
			return EeHooks::Action::Continue;
		}

		EeHooks::Action OnRng(u32 pc)
		{
			const u32 ra = cpuRegs.GPR.n.ra.UL[0];
			if (s_man.sound_sites.count(ra - 8) && s_man.sound_seed)
			{
				// sound-only draw from its own stream in rolled-back EE memory: never g_RandSeed (the sim stream stays
				// independent of audio), yet deterministic and rewound like the rest of the game (voice picks feed
				// voice lengths, which feed the simulation through the virtual stream clock)
				const u32 seed = Rd(s_man.sound_seed) * 214013u + 2531011u;
				Wr(s_man.sound_seed, seed);
				if (pc == s_man.rng_float)
				{
					float lo, hi;
					std::memcpy(&lo, &fpuRegs.fpr[12].UL, 4);
					std::memcpy(&hi, &fpuRegs.fpr[13].UL, 4);
					const float r = lo + (hi - lo) * (static_cast<float>(seed >> 16) / 65536.0f);
					std::memcpy(&fpuRegs.fpr[0].UL, &r, 4);
				}
				else
				{
					const s32 lo = cpuRegs.GPR.n.a0.SL[0], hi = cpuRegs.GPR.n.a1.SL[0];
					cpuRegs.GPR.n.v0.SD[0] = lo + static_cast<s32>((static_cast<s64>(seed >> 16) * (hi - lo)) >> 16);
				}
				return EeHooks::Action::Return;
			}
			if (s_man.sound_sites.count(ra - 8))
			{
				// sound-only draw: the host sound stream, never g_RandSeed (sim stream independent of audio)
				if (pc == s_man.rng_float)
					fpuRegs.fpr[0].UL = static_cast<u32>(RollbackDevice::HandleSyscall(
						RollbackDevice::CMD_SND_RAND_FLOAT, fpuRegs.fpr[12].UL, fpuRegs.fpr[13].UL, 0));
				else
					cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(RollbackDevice::HandleSyscall(
						RollbackDevice::CMD_SND_RAND_INT, cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0], 0));
				return EeHooks::Action::Return;
			}
			if (RollbackDevice::TraceOn())
			{
				const auto it = s_man.trace_ids.find(pc);
				if (it != s_man.trace_ids.end())
					RollbackDevice::TraceCall(it->second, ra, cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
						cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0]);
			}
			return EeHooks::Action::Continue;
		}
		EeHooks::Action OnTrace(u32 pc)
		{
			if (RollbackDevice::TraceOn())
			{
				const auto it = s_man.trace_ids.find(pc);
				if (it != s_man.trace_ids.end())
					RollbackDevice::TraceCall(it->second, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0],
						cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0]);
			}
			return EeHooks::Action::Continue;
		}

		// ---------------------------------------------------------------------------------------------------------
		// install / configure (EE thread, at the sim-tick site)
		// ---------------------------------------------------------------------------------------------------------
		void InstallAttachHooks()
		{
			EeHooks::AddCall(s_man.sim_tick_site, OnSimTickSite, EeHooks::OWNER_GAME);
			EeHooks::AddCall(s_man.return_addr, OnReturnAddr, EeHooks::OWNER_GAME);
			if (s_man.pad_read_fn && s_man.pad_site)
			{
				EeHooks::AddCall(s_man.pad_read_fn, OnPadRead, EeHooks::OWNER_GAME);
				EeHooks::AddCall(s_man.pad_site + 8, OnPadReadReturn, EeHooks::OWNER_GAME);
			}
			if (s_man.replay_fn && s_man.replay_site)
			{
				EeHooks::AddCall(s_man.replay_fn, OnReplayEntry, EeHooks::OWNER_GAME);
				EeHooks::AddCall(s_man.replay_site + 8, OnReplayReturn, EeHooks::OWNER_GAME);
			}
		}
		// ---------------------------------------------------------------------------------------------------------
		// virtual stream clock (manifest `virtual_streams`): the simulation's view of stream/voice playback answered
		// from a deterministic model in rolled-back EE memory instead of the real audio (FUC notes/M4_AUDIO_CLOCK_DESIGN.md)
		// ---------------------------------------------------------------------------------------------------------
		constexpr u32 VS_MAGIC = 0x31435356u; // 'VSC1'
		constexpr u32 VS_STARTED = 1, VS_STOPPED = 2, VS_PAUSED = 4, VS_FADEOUT = 8, VS_LOOPS = 0x10;
		enum VsField : u32
		{
			VS_ID = 0x00, VS_PRIO = 0x04, VS_FLAGS = 0x08, VS_REQ = 0x0C, VS_LEN = 0x10, VS_LSTART = 0x14, VS_LEND = 0x18,
			VS_PBEGIN = 0x1C, VS_PACC = 0x20, VS_FADE_END = 0x24, VS_PART = 0x28,
		};
		u32 VsCh(u32 ch, u32 field) { return s_man.vs.state + 0x10 + ch * 0x30 + field; }
		u32 VsNow() { return Rd(s_man.vs.state + 4); }
		bool VsEnabled() { return s_man.vs.state && Rd(s_man.vs.state) == VS_MAGIC && Rd(s_man.vs.state + 12) != 0; }
		bool VsReady() { return !s_man.vs.ready_addr || Rd(s_man.vs.ready_addr) != 0; }
		s64 VsP0(u32 ch) { return static_cast<s64>(Rd(VsCh(ch, VS_REQ))) + Rd(s_man.vs.state + 8); }
		s64 VsPlay(u32 ch, s64 n)
		{
			const s64 p0 = VsP0(ch);
			const u32 fl = Rd(VsCh(ch, VS_FLAGS));
			s64 play = std::max<s64>(0, n - p0) - Rd(VsCh(ch, VS_PACC));
			if (fl & VS_PAUSED)
				play -= std::max<s64>(0, n - std::max<s64>(Rd(VsCh(ch, VS_PBEGIN)), p0));
			return play;
		}
		u32 VsStat(u32 ch)
		{
			if (ch >= s_man.vs.channels)
				return 0;
			const s64 n = VsNow();
			const u32 fl = Rd(VsCh(ch, VS_FLAGS));
			if (!(fl & VS_STARTED) || (fl & VS_STOPPED))
				return 0;
			if ((fl & VS_FADEOUT) && n >= static_cast<s64>(Rd(VsCh(ch, VS_FADE_END))))
				return 0;
			if (n < VsP0(ch))
				return 1;
			if (!(fl & VS_LOOPS) && VsPlay(ch, n) >= static_cast<s64>(Rd(VsCh(ch, VS_LEN))))
				return 5;
			return 3;
		}
		bool VsActive(u32 ch)
		{
			const u32 s = VsStat(ch);
			return s >= 1 && s <= 4;
		}
		s64 VsValue(const std::string& what, u32 ch)
		{
			if (what == "stat")
				return VsStat(ch);
			if (what == "busy")
				return VsActive(ch) ? 1 : 0;
			if (what == "id")
				return VsActive(ch) ? static_cast<s32>(Rd(VsCh(ch, VS_ID))) : -1;
			if (what == "timeframes")
			{
				const u32 s = VsStat(ch);
				return s == 3 ? VsPlay(ch, VsNow()) : s == 5 ? Rd(VsCh(ch, VS_LEN)) : 0;
			}
			if (what == "lpcnt")
			{
				const u32 fl = Rd(VsCh(ch, VS_FLAGS));
				const s64 play = VsPlay(ch, VsNow()), ls = Rd(VsCh(ch, VS_LSTART)), le = Rd(VsCh(ch, VS_LEND));
				return ((fl & VS_LOOPS) && play >= le && le > ls) ? 1 + (play - le) / (le - ls) : 0;
			}
			return 0;
		}
		void VsRequest(u32 ch, u32 idx, s32 prio, bool paused)
		{
			if (ch >= s_man.vs.channels)
				return;
			const u32 part = ch != 0 ? 1 : 0;
			const auto it = s_man.vs.lengths.find((static_cast<u64>(part) << 32) | idx);
			if (it == s_man.vs.lengths.end())
				return; // the game's start path would find no such stream either
			const bool active = VsActive(ch);
			const bool faded = active && (Rd(VsCh(ch, VS_FLAGS)) & VS_FADEOUT);
			const s32 eff_prio = active ? static_cast<s32>(Rd(VsCh(ch, VS_PRIO))) : 99;
			if (active && eff_prio < prio && !faded)
				return; // the game drops a lower-priority request on a busy channel
			const u32 now = VsNow();
			Wr(VsCh(ch, VS_ID), idx);
			Wr(VsCh(ch, VS_PRIO), static_cast<u32>(prio));
			Wr(VsCh(ch, VS_FLAGS), VS_STARTED | (paused ? VS_PAUSED : 0) | (it->second.loops ? VS_LOOPS : 0));
			Wr(VsCh(ch, VS_REQ), now);
			Wr(VsCh(ch, VS_LEN), it->second.frames);
			Wr(VsCh(ch, VS_LSTART), it->second.loop_start);
			Wr(VsCh(ch, VS_LEND), it->second.loop_end);
			Wr(VsCh(ch, VS_PBEGIN), now);
			Wr(VsCh(ch, VS_PACC), 0);
			Wr(VsCh(ch, VS_FADE_END), 0);
			Wr(VsCh(ch, VS_PART), part);
		}
		void VsKill(u32 ch)
		{
			if (ch < s_man.vs.channels)
				Wr(VsCh(ch, VS_FLAGS), (Rd(VsCh(ch, VS_FLAGS)) | VS_STOPPED) & ~(VS_PAUSED | VS_FADEOUT));
		}
		void VsPause(u32 ch)
		{
			if (ch < s_man.vs.channels && VsActive(ch) && !(Rd(VsCh(ch, VS_FLAGS)) & VS_PAUSED))
			{
				Wr(VsCh(ch, VS_FLAGS), Rd(VsCh(ch, VS_FLAGS)) | VS_PAUSED);
				Wr(VsCh(ch, VS_PBEGIN), VsNow());
			}
		}
		void VsResume(u32 ch)
		{
			if (ch >= s_man.vs.channels || !(Rd(VsCh(ch, VS_FLAGS)) & VS_PAUSED))
				return;
			const s64 add = std::max<s64>(0, static_cast<s64>(VsNow()) - std::max<s64>(Rd(VsCh(ch, VS_PBEGIN)), VsP0(ch)));
			Wr(VsCh(ch, VS_PACC), Rd(VsCh(ch, VS_PACC)) + static_cast<u32>(add));
			Wr(VsCh(ch, VS_FLAGS), Rd(VsCh(ch, VS_FLAGS)) & ~VS_PAUSED);
		}
		void VsFade(u32 ch, s32 dur, float target)
		{
			if (ch >= s_man.vs.channels || !VsActive(ch))
				return;
			if (target < 1e-4f)
			{
				Wr(VsCh(ch, VS_FLAGS), Rd(VsCh(ch, VS_FLAGS)) | VS_FADEOUT);
				Wr(VsCh(ch, VS_FADE_END), VsNow() + static_cast<u32>(std::max(dur, 1)));
			}
			else
				Wr(VsCh(ch, VS_FLAGS), Rd(VsCh(ch, VS_FLAGS)) & ~VS_FADEOUT);
		}
		void VsInit()
		{
			const VirtualStreams& v = s_man.vs;
			if (!v.state)
				return;
			for (u32 a = 0; a < 0x10 + v.channels * 0x30; a += 4)
				Wr(v.state + a, 0);
			Wr(v.state, VS_MAGIC);
			Wr(v.state + 8, v.k_prep);
			Wr(v.state + 12, 1);
			for (u32 ch = 0; ch < v.channels; ch++)
			{
				Wr(VsCh(ch, VS_ID), 0xFFFFFFFFu);
				Wr(VsCh(ch, VS_PRIO), 99);
			}
		}
		u64 RegRead(int r) { return r >= 100 ? fpuRegs.fpr[r - 100].UL : cpuRegs.GPR.r[r].UD[0]; }

		// ---------------------------------------------------------------------------------------------------------
		// hook chains: several behaviours at one address run in order (the first that returns / jumps wins), then an
		// optional resim gate. A lone gate / lone filtered call is emitted natively.
		// ---------------------------------------------------------------------------------------------------------
		struct ChainAction
		{
			std::vector<u32> ra; // empty = any caller
			std::function<EeHooks::Action()> fn;
		};
		struct Chain
		{
			std::vector<ChainAction> actions;
			int gate = 0; // 1 gate, 2 gate that sets v0
			u32 gate_v0 = 0;
		};
		std::map<u32, Chain> s_chains;
		std::vector<u32> s_installed; // every address the rollback hooks own (removed on stop)

		void ChainAdd(u32 pc, std::vector<u32> ra, std::function<EeHooks::Action()> fn)
		{
			s_chains[pc].actions.push_back({std::move(ra), std::move(fn)});
		}
		EeHooks::Action RunChain(u32 pc)
		{
			const auto it = s_chains.find(pc);
			if (it == s_chains.end())
				return EeHooks::Action::Continue;
			const u32 ra = cpuRegs.GPR.n.ra.UL[0];
			for (const ChainAction& a : it->second.actions)
			{
				bool hit = a.ra.empty();
				for (const u32 r : a.ra)
					hit |= (r == ra);
				if (!hit)
					continue;
				const EeHooks::Action act = a.fn();
				if (act != EeHooks::Action::Continue)
					return act;
			}
			if (it->second.gate && RollbackDevice::IsResimulating())
			{
				if (it->second.gate == 2)
					cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(it->second.gate_v0);
				return EeHooks::Action::Return;
			}
			return EeHooks::Action::Continue;
		}

		void BuildChains()
		{
			s_chains.clear();
			if (s_man.render_begin_site)
				ChainAdd(s_man.render_begin_site, {}, [] {
					RollbackDevice::HandleSyscall(RollbackDevice::CMD_RENDER_BEGIN, 0, 0, 0);
					return EeHooks::Action::Continue;
				});
			if (s_man.render_end_site)
				ChainAdd(s_man.render_end_site, {}, [] {
					RollbackDevice::HandleSyscall(RollbackDevice::CMD_RENDER_END, 0, 0, 0);
					return EeHooks::Action::Continue;
				});
			for (size_t k = 0; k < s_man.entry_actions.size(); k++)
			{
				ChainAdd(s_man.entry_actions[k].at, {}, [k] {
					const EntryAction& a = s_man.entry_actions[k];
					if (a.resim_only && !RollbackDevice::IsResimulating())
						return EeHooks::Action::Continue;
					for (const Op& op : a.ops)
						DoOp(op);
					if (!a.ret)
						return EeHooks::Action::Continue;
					cpuRegs.GPR.n.v0.UD[0] = 0;
					return EeHooks::Action::Return;
				});
			}
			// sound-only draws: the host sound stream (only those callers); trace sees every other call
			std::vector<u32> callers;
			for (const u32 site : s_man.sound_sites)
				callers.push_back(site + 8);
			for (const u32 fn : {s_man.rng_float, s_man.rng_int})
				if (fn && !callers.empty())
					ChainAdd(fn, callers, [fn] { return OnRng(fn); });
			if (RollbackDevice::TraceOn())
				for (const auto& [fn, id] : s_man.trace_ids)
					if (fn != s_man.pad_read_fn && fn != s_man.sim_tick_site && fn != s_man.return_addr)
						ChainAdd(fn, {}, [fn] { return OnTrace(fn); });
			// virtual stream clock
			const VirtualStreams& v = s_man.vs;
			if (v.state)
			{
				for (const VsHook& h : v.hooks)
				{
					const VsHook hk = h;
					ChainAdd(h.at, h.ra, [hk] {
						if (!VsEnabled())
							return EeHooks::Action::Continue;
						const u32 ch = static_cast<u32>(RegRead(hk.ch_reg) & (hk.kind == "request" ? hk.ch_mask : 0xFFFFFFFFu));
						if (hk.kind == "tick")
							Wr(s_man.vs.state + 4, VsNow() + 1);
						else if (hk.kind == "request")
						{
							if (VsReady())
								VsRequest(ch, static_cast<u32>(RegRead(hk.idx_reg)), static_cast<s16>(RegRead(hk.prio_reg)),
									static_cast<s16>(RegRead(hk.paused_reg)) != 0);
						}
						else if (hk.kind == "kill")
						{
							if (!hk.need_ready || VsReady())
								VsKill(ch);
						}
						else if (hk.kind == "pause")
						{
							if (!hk.need_ready || VsReady())
								VsPause(ch);
						}
						else if (hk.kind == "resume")
						{
							if (!hk.need_ready || VsReady())
								VsResume(ch);
						}
						else if (hk.kind == "fade")
						{
							if (VsReady())
							{
								const u32 bits = static_cast<u32>(RegRead(hk.target_reg));
								float target;
								std::memcpy(&target, &bits, 4);
								VsFade(ch, static_cast<s32>(RegRead(hk.dur_reg)), target);
							}
						}
						else if (hk.kind == "query" || hk.kind == "post")
						{
							const s64 v = (hk.kind == "query" && hk.need_ready && !VsReady()) ? hk.not_ready : VsValue(hk.value, ch);
							cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(v);
							if (hk.kind == "query")
								return EeHooks::Action::Return;
						}
						return EeHooks::Action::Continue;
					});
				}
			}
			for (const u32 a : s_man.resim_gates)
			{
				Chain& c = s_chains[a];
				const auto v = s_man.gate_values.find(a);
				c.gate = v != s_man.gate_values.end() ? 2 : 1;
				c.gate_v0 = v != s_man.gate_values.end() ? v->second : 0;
			}
		}

		void InstallRollbackHooks()
		{
			BuildChains();
			s_installed.clear();
			for (const auto& [pc, c] : s_chains)
			{
				if (c.actions.empty())
				{
					if (c.gate == 2)
						EeHooks::AddResimGateRet(pc, c.gate_v0, EeHooks::OWNER_GAME);
					else
						EeHooks::AddResimGate(pc, EeHooks::OWNER_GAME);
				}
				else if (c.actions.size() == 1 && !c.gate && !c.actions[0].ra.empty())
				{
					const auto fn = c.actions[0].fn;
					EeHooks::AddCallFiltered(pc, [fn](u32) { return fn(); }, c.actions[0].ra, EeHooks::OWNER_GAME);
				}
				else
					EeHooks::AddCall(pc, RunChain, EeHooks::OWNER_GAME);
				s_installed.push_back(pc);
			}
			for (const u32 a : s_man.resim_skips)
			{
				if (s_chains.count(a))
					Console.Error("GameRollback: %08X is both a call-site skip and a hook: skip ignored", a);
				else
				{
					EeHooks::AddSkipCall(a, false, EeHooks::OWNER_GAME);
					s_installed.push_back(a);
				}
			}
			for (const u32 a : s_man.always_skips)
			{
				EeHooks::AddSkipCall(a, true, EeHooks::OWNER_GAME);
				s_installed.push_back(a);
			}
		}
		void InstallTraceHooks() {} // part of the chains (BuildChains)
		void RemoveRollbackHooks()
		{
			for (const u32 a : s_installed)
				EeHooks::Remove(a);
			s_installed.clear();
			s_chains.clear();
		}

		void ConfigureDevice()
		{
			using namespace RollbackDevice;
			ClearConfig();
			if (s_man.vs.state) // the virtual stream clock is simulation state: rolled back and compared
				AddRegion(s_man.vs.state, 0x10 + s_man.vs.channels * 0x30);
			if (s_man.sound_seed)
				AddRegion(s_man.sound_seed, 4);
			for (const RegionSpec& r : s_man.regions)
			{
				s64 len = 0;
				if (r.has_end)
				{
					s64 end = 0;
					if (Eval(r.end, 0, &end) && end > r.addr)
						len = end - r.addr;
				}
				else
					Eval(r.len, 0, &len);
				if (len > 0)
					AddRegion(r.addr, static_cast<u32>(len));
				else
					Console.Error("GameRollback: region %08X has no valid length/end", r.addr);
			}
			for (const Item& it : s_man.excludes)
				if (const std::optional<u32> a = Resolve(it.where))
					AddExclude(*a, it.len);
			if (s_man.input_block.len)
				SetInputBlock(s_man.input_block.addr, s_man.input_block.len);
			for (const Item& it : s_man.ignores)
				if (const std::optional<u32> a = Resolve(it.where))
					AddCompareIgnore(*a, it.len);
			if (s_man.gate_counter)
				SetGate(s_man.gate_counter);
			for (const Range& r : s_man.gate_stable)
				AddGateStable(r.addr, r.len);
			for (const Range& r : s_man.resim_restore)
				AddResimRestore(r.addr, r.len);
			SetResimFlagAddr(0);
			if (s_man.rng_split.len)
				SetRngSplit(s_man.rng_split.addr, s_man.rng_split.len);
			s_hash_ranges.clear();
			for (const Item& it : s_man.watches)
				if (const std::optional<u32> a = Resolve(it.where))
				{
					AddWatch(*a, it.len, it.name);
					s_hash_ranges.emplace_back(*a, it.len);
				}
			s_dyn_sig = 0;
			RefreshDynamic(true);
		}

		bool LoadFile(const std::string& path, Manifest* m, std::string* error)
		{
			std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
			if (!text.has_value())
			{
				if (error)
					*error = fmt::format("cannot read {}", path);
				return false;
			}
			return ParseManifest(*text, path, m, error);
		}
		s64 MTime(const std::string& path)
		{
			FILESYSTEM_STAT_DATA sd;
			return FileSystem::StatFile(path.c_str(), &sd) ? sd.ModificationTime : 0;
		}

		void DoStop()
		{
			RollbackDevice::Stop();
			RemoveRollbackHooks();
			s_mode = 0;
			s_driving = s_passthrough = false;
		}
		void DoStart(int mode, u32 frames)
		{
			if (s_mode != 0)
				DoStop();
			ConfigureDevice();
			VsInit(); // both peers start the clock from the same (empty) state at the rollback start
			if (s_man.sound_seed)
				Wr(s_man.sound_seed, 0x5DB2A5D1u); // every peer starts the sound stream from the same seed
			InstallRollbackHooks();
			InstallTraceHooks();
			RollbackDevice::Start(static_cast<RollbackDevice::Mode>(mode), frames, true);
			s_host_frame = -1;
			for (auto& row : s_pad_hist)
				for (PadRec& r : row)
					r.frame = -1;
			s_mode = mode;
			s_frames = frames;
		}
		void DoDetach()
		{
			if (s_mode != 0)
				DoStop();
			EeHooks::Clear(EeHooks::OWNER_GAME);
			s_attached = false;
			s_status = "gamerb: detached";
		}
		bool DoAttach(const std::string& path)
		{
			Manifest m;
			std::string err;
			if (!LoadFile(path, &m, &err))
			{
				s_status = fmt::format("gamerb: {} failed: {}", path, err);
				Console.Error("GameRollback: %s", s_status.c_str());
				return false;
			}
			const int mode = s_mode;
			const u32 frames = s_frames;
			if (s_attached)
				DoDetach();
			s_man = std::move(m);
			s_macros = s_man.macros;
			s_path = path;
			s_mtime = MTime(path);
			InstallAttachHooks();
			s_attached = true;
			if (!s_watchdog_run.exchange(true))
				s_watchdog = std::thread(WatchdogLoop), s_watchdog.detach();
			s_status = fmt::format("gamerb: {} ({})", s_man.name, path);
			Console.WriteLn("GameRollback: loaded %s (%zu steps, %zu gates, %zu skips)", path.c_str(), s_man.steps.size(),
				s_man.resim_gates.size(), s_man.resim_skips.size());
			if (mode != 0)
				DoStart(mode, frames); // hot reload keeps the running mode
			return true;
		}

		void ApplyRequests()
		{
			// hot reload: poll the manifest's modification time about twice a second
			if (s_watch && s_attached && ++s_watch_tick >= 30)
			{
				s_watch_tick = 0;
				const s64 mt = MTime(s_path);
				if (mt && mt != s_mtime)
				{
					Console.WriteLn("GameRollback: manifest changed, reloading");
					DoAttach(s_path);
				}
			}
			Request r;
			{
				std::lock_guard lk(s_req_mtx);
				if (!s_req_pending)
					return;
				r = s_req;
				s_req = {};
				s_req_pending = false;
			}
			if (r.detach)
				DoDetach();
			if (r.attach)
				DoAttach(r.path);
			if (r.stop)
				DoStop();
			if (r.start && s_attached)
				DoStart(r.mode, r.frames);
			if (r.net_stop && s_net)
			{
				NetBridge::Stop();
				s_net = false;
				DoStop();
			}
			if (r.net_start && s_attached)
			{
				NetBridge::Host host;
				host.poll_local_input = [](int player, u8* out) {
					static constexpr u8 NEUTRAL[NetBridge::INPUT_SIZE] = {0, 0, 0x80, 0x80, 0x80, 0x80};
					if (player >= 0 && player < static_cast<int>(PAD_PLAYERS) && s_live_valid[player])
						ReportToInput(s_live_report[player], out);
					else
						std::memcpy(out, NEUTRAL, sizeof(NEUTRAL));
				};
				host.in_game = [] {
					s64 v = 1;
					return !s_man.gate_when || (Eval(*s_man.gate_when, 0, &v) && v != 0);
				};
				std::string err;
				if (!NetBridge::Start(r.net, host, &err))
					Console.Error("GameRollback: netplay start failed: %s", err.c_str());
				else
				{
					DoStart(static_cast<int>(RollbackDevice::Mode::Netplay), 8); // rollback depth fixed at 8
					s_net = true;
					s_net_fwd_save = -1;
					for (bool& v : s_live_valid)
						v = false;
				}
			}
		}
	} // namespace

	// The first request (attach) has to reach the EE thread before the sim-tick hook exists: it registers only that
	// hook here, and everything else happens at the next frame boundary on the EE thread.
	bool Attach(const std::string& manifest_path, std::string* error)
	{
		Manifest probe;
		if (!LoadFile(manifest_path, &probe, error))
			return false;
		{
			std::lock_guard lk(s_req_mtx);
			s_req.attach = true;
			s_req.path = manifest_path;
			s_req_pending = true;
		}
		EeHooks::AddCall(probe.sim_tick_site, OnSimTickSite, EeHooks::OWNER_GAME);
		return true;
	}
	void Detach()
	{
		std::lock_guard lk(s_req_mtx);
		s_req.detach = true;
		s_req_pending = true;
	}
	bool IsAttached() { return s_attached; }

	bool Start(int mode, u32 frames, std::string* error)
	{
		std::lock_guard lk(s_req_mtx);
		if (!s_attached && !s_req.attach)
		{
			if (error)
				*error = "no manifest attached";
			return false;
		}
		s_req.start = true;
		s_req.stop = false;
		s_req.mode = mode;
		s_req.frames = frames;
		s_req_pending = true;
		return true;
	}
	void Stop()
	{
		std::lock_guard lk(s_req_mtx);
		s_req.stop = true;
		s_req.start = false;
		s_req_pending = true;
	}
	int RunningMode() { return s_mode; }

	bool NetStart(int mode, int local_player, const std::string& remote, u16 port, u8 delay, const std::string& replay,
		std::string* error)
	{
		std::lock_guard lk(s_req_mtx);
		if (!s_attached && !s_req.attach)
		{
			if (error)
				*error = "no manifest attached";
			return false;
		}
		s_req.net_start = true;
		s_req.net.mode = static_cast<NetBridge::Mode>(mode);
		s_req.net.local_player = local_player;
		s_req.net.remote = remote;
		s_req.net.port = port;
		s_req.net.input_delay = delay;
		s_req.net.replay_path = replay;
		s_req.net.game_id = s_man.serial;
		s_req_pending = true;
		return true;
	}
	void NetStop()
	{
		std::lock_guard lk(s_req_mtx);
		s_req.net_stop = true;
		s_req_pending = true;
	}
	std::string NetStatus() { return NetBridge::Status() + fmt::format(" | refused rollbacks {}", s_net_refused); }
	void OnStateLoaded()
	{
		// a savestate never lands inside a re-simulation, but the driver must not carry one over either
		s_driving = s_passthrough = s_pad_pending = s_replay_pending = false;
	}
	std::string Status() { return s_status + fmt::format(" | mode {}", s_mode); }
	void SetFileWatch(bool on) { s_watch = on; }
} // namespace GameRollback
