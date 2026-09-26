// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/GameRollback.h"
#include "Sdbz/EeHooks.h"
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

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
		struct Manifest
		{
			std::string serial, name;
			std::vector<RegionSpec> regions;
			std::unordered_map<std::string, std::string> macros;
			std::optional<Val> gate_when;
			std::unordered_map<u32, u32> gate_values; // resim gates that also set $v0
			bool exclude_thread_stacks = true;
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
			u32 rng_float = 0, rng_int = 0;
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
			if (Has(root, "pad"))
			{
				m->pad_read_fn = Get(Child(root, "pad"), "read_fn", 0);
				m->pad_site = Get(Child(root, "pad"), "site", 0);
				m->pad_record_replay = GetStr(Child(root, "pad"), "record_replay") == "true";
			}
			if (Has(root, "rng"))
			{
				const auto r = Child(root, "rng");
				m->rng_float = Get(r, "range_float", 0);
				m->rng_int = Get(r, "range_int", 0);
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
			bool attach = false, detach = false, start = false, stop = false;
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

		// dynamic ranges
		u64 s_dyn_sig = 0;

		// hot reload
		bool s_watch = true;
		s64 s_mtime = 0;
		u32 s_watch_tick = 0;

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
			if (s_man.exclude_thread_stacks && CurrentBiosInformation.eeThreadListAddr)
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

		EeHooks::Action OnSimTickSite(u32)
		{
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
			s_host_frame++;
			const u32 R = static_cast<u32>(RollbackDevice::HandleSyscall(RollbackDevice::CMD_FRAME_BEGIN, 0, 0, 0));
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
			if (cpuRegs.GPR.n.ra.UL[0] == s_man.pad_site + 8 && s_man.pad_record_replay && s_driving)
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
			if (s_man.pad_record_replay && s_driving)
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
			if (s_man.pad_record_replay && s_mode != 0 && s_host_frame >= 0)
			{
				PadRec& r = s_pad_hist[static_cast<u32>(s_host_frame) % PAD_HIST][player];
				r.frame = s_host_frame;
				r.ret = cpuRegs.GPR.n.v0.UL[0];
				std::memcpy(r.report, buf, PAD_REPORT);
			}
			return EeHooks::Action::Continue;
		}

		EeHooks::Action OnRng(u32 pc)
		{
			const u32 ra = cpuRegs.GPR.n.ra.UL[0];
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
		}
		void InstallRollbackHooks()
		{
			if (s_man.render_begin_site)
				EeHooks::AddCall(s_man.render_begin_site, [](u32) {
					RollbackDevice::HandleSyscall(RollbackDevice::CMD_RENDER_BEGIN, 0, 0, 0);
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
			if (s_man.render_end_site)
				EeHooks::AddCall(s_man.render_end_site, [](u32) {
					RollbackDevice::HandleSyscall(RollbackDevice::CMD_RENDER_END, 0, 0, 0);
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
			for (const u32 a : s_man.resim_gates)
			{
				const auto v = s_man.gate_values.find(a);
				if (v != s_man.gate_values.end())
					EeHooks::AddResimGateRet(a, v->second, EeHooks::OWNER_GAME);
				else
					EeHooks::AddResimGate(a, EeHooks::OWNER_GAME);
			}
			for (const u32 a : s_man.resim_skips)
				EeHooks::AddSkipCall(a, false, EeHooks::OWNER_GAME);
			for (const u32 a : s_man.always_skips)
				EeHooks::AddSkipCall(a, true, EeHooks::OWNER_GAME);
			for (size_t k = 0; k < s_man.entry_actions.size(); k++)
			{
				EeHooks::AddCall(s_man.entry_actions[k].at, [k](u32) {
					const EntryAction& a = s_man.entry_actions[k];
					if (a.resim_only && !RollbackDevice::IsResimulating())
						return EeHooks::Action::Continue;
					for (const Op& op : a.ops)
						DoOp(op);
					if (!a.ret)
						return EeHooks::Action::Continue;
					cpuRegs.GPR.n.v0.UD[0] = 0;
					return EeHooks::Action::Return;
				}, EeHooks::OWNER_GAME);
			}
			// sound-only draws: only those callers reach the handler (native $ra filter), unless call tracing wants all
			std::vector<u32> callers;
			for (const u32 site : s_man.sound_sites)
				callers.push_back(site + 8);
			for (const u32 fn : {s_man.rng_float, s_man.rng_int})
			{
				if (!fn)
					continue;
				if (RollbackDevice::TraceOn())
					EeHooks::AddCall(fn, OnRng, EeHooks::OWNER_GAME);
				else
					EeHooks::AddCallFiltered(fn, OnRng, callers, EeHooks::OWNER_GAME);
			}
		}
		void InstallTraceHooks()
		{
			for (const auto& [fn, id] : s_man.trace_ids)
				if (fn != s_man.rng_float && fn != s_man.rng_int && fn != s_man.pad_read_fn)
					EeHooks::AddCall(fn, OnTrace, EeHooks::OWNER_GAME);
		}
		void RemoveRollbackHooks()
		{
			// everything except the attach hooks
			if (s_man.render_begin_site)
				EeHooks::Remove(s_man.render_begin_site);
			if (s_man.render_end_site)
				EeHooks::Remove(s_man.render_end_site);
			for (const u32 a : s_man.resim_gates)
				EeHooks::Remove(a);
			for (const u32 a : s_man.resim_skips)
				EeHooks::Remove(a);
			for (const u32 a : s_man.always_skips)
				EeHooks::Remove(a);
			for (const EntryAction& a : s_man.entry_actions)
				EeHooks::Remove(a.at);
			if (s_man.rng_float)
				EeHooks::Remove(s_man.rng_float);
			if (s_man.rng_int)
				EeHooks::Remove(s_man.rng_int);
			for (const auto& [fn, id] : s_man.trace_ids)
				if (fn != s_man.pad_read_fn && fn != s_man.sim_tick_site && fn != s_man.return_addr)
					EeHooks::Remove(fn);
		}

		void ConfigureDevice()
		{
			using namespace RollbackDevice;
			ClearConfig();
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
			for (const Item& it : s_man.watches)
				if (const std::optional<u32> a = Resolve(it.where))
					AddWatch(*a, it.len, it.name);
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
	void OnStateLoaded()
	{
		// a savestate never lands inside a re-simulation, but the driver must not carry one over either
		s_driving = s_passthrough = s_pad_pending = false;
	}
	std::string Status() { return s_status + fmt::format(" | mode {}", s_mode); }
	void SetFileWatch(bool on) { s_watch = on; }
} // namespace GameRollback
