// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/GameRollback.h"
#include "Sdbz/CssMirror.h"
#include "Sdbz/EeHooks.h"
#include "Sdbz/NetBridge.h"
#include "Sdbz/PcInput.h"
#include "Sdbz/RollbackDevice.h"

#include "DebugTools/BiosDebugData.h"
#include "Host.h"
#include "Memory.h"
#include "R5900.h"
#include "ps2/BiosTools.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/YAML.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <deque>
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
			// session lock-down (netplay sessions only): forced branches and state-conditioned input masks
			struct ForcedBranch
			{
				u32 at = 0, to = 0;
				std::vector<std::pair<int, u32>> regs; // delay-slot effects of the taken branch (reg, value)
			};
			struct InputMask
			{
				u32 player = 0;
				Val when;
				u16 buttons = 0; // PadFeed layout ((b2 << 8) | b3, active-high)
			};
			std::vector<ForcedBranch> session_branches;
			std::vector<InputMask> session_masks;
			// load-time data patches of game scripts: at `hook` (a loader entry), file = GPR[file_reg]; a group applies only
			// when every `expect` matches, so a patch can never land on a different script or version
			struct ScriptPatch
			{
				u32 off = 0;
				std::vector<u8> expect, write;
			};
			struct ScriptPatchGroup
			{
				std::string name;
				std::vector<ScriptPatch> patches;
			};
			// Async menus + per-battle rollback (session.link; bridge link mode). Menus run locally, only menu events travel.
			struct LinkHook
			{
				u32 at = 0;
				int reg = 5;
				std::string match; // string prefix at GPR[reg] (empty = none)
				bool has_value = false;
				u32 value = 0;     // or: GPR[reg] == value
			};
			struct ExprWrite
			{
				Val addr, value;
			};
			struct LinkSpec
			{
				bool on = false;
				LinkHook attach, detach, commit;          // battle attach point, match end (one-more open), selection commit
				std::vector<std::pair<u32, int>> selection; // resolved picks: address, owning player (0/1)
				u32 seed_addr = 0;                          // written from player 0's value at commit
				std::vector<ExprWrite> attach_writes;       // canonicalization at every (re-)attach
				std::vector<ExprWrite> commit_writes;       // canonicalization at the commit (menu-async state that drives the
				                                            // commit -> attach window: effect/sound RNG streams, ...)
				Val onemore_decided;                        // the local one-more choice has been made
				u32 onemore_choice = 0, retry_value = 1;    // choice word, its RETRY value
				// the one-more menu as a vote: the local ○ is withheld and sent; once both votes are in, the host moves the
				// menu cursor (a script ctx var) to the agreed option and confirms -- the menu stays live meanwhile
				u32 vote_sig_off = 0, vote_cursor_var = 0, vote_retry_index = 0, vote_css_index = 1;
				std::string vote_sig;
				std::vector<ExprWrite> retry_writes, css_writes; // apply the agreed choice
				// pre-attach hold (async-menus option B): between the commit and the attach, a frame where the game's
				// load request list has pending requests skips the sim/render (jump at -> to), so every load costs the
				// same number of simulation ticks on every peer regardless of disc timing
				u32 pend_list = 0, pend_pool = 0, pend_done_bit = 0x100, pend_next_off = 8, pend_sentinel = 1;
				std::vector<std::pair<u32, u32>> hold_jumps;
				std::vector<u32> hold_wait_to;              // per jump: target while waiting for the peer (0 = same as `to`)
				u32 present_at = 0, present_to = 0;         // held frames skip the buffer flip (+ back-buffer clear): the last
				                                            // complete frame stays on screen instead of stale buffers flickering
				// virtual load clock (pre-attach window): the game's loader sees each read complete a FIXED number of
				// loader ticks after it was issued (base + sectors / sectors_per_tick), identical on both peers, so the
				// VS screen keeps animating while the disc works. A frame is held only when the virtual deadline has
				// come and the real read is still running (stall_to skips the sim tick AND the loader tick).
				struct VirtualLoad
				{
					u32 poll_at = 0;    // return site of the loader's single "read finished?" poll (v0 = real answer)
					u32 req_reg = 17;   // register holding the request there
					u32 state_off = 32, poll_state = 5; // request loader state while polling
					u32 handle_off = 44, stat_off = 1, ready = 3; // ADXF handle, its status byte, "ready" value
					u32 sectors_off = 52;
					u32 base = 2, sectors_per_tick = 64;
					u32 stall_to = 0;   // first hold jump's target on a stall frame
				} vload;
			} link;
			CssMirror::Config css_mirror; // link.css_mirror
			// link.fx_canon: effect-pool free lists rebuilt in their pool-init order at every attach (FUC notes/RETRY_CANON.md)
			struct FreeList
			{
				u32 head = 0, stride = 0;
			};
			bool fx_canon = false;
			std::vector<FreeList> fx_dlists, fx_slists; // doubly linked FIFO {1, last, first} / singly linked LIFO (top = highest)
			std::vector<u32> fx_at;                     // also at these pcs (pools empty: the RETRY kill), pre-attach only
			std::vector<ExprWrite> fx_at_writes;        // ... together with these writes
			// link.ai_wait_canon: AI-script "wait for FIGHT or N frames" countdowns restarted at the attach
			struct AiWaitLoop
			{
				u32 set = 0, yield_pc = 0, reg = 20, value = 600;
			};
			bool ai_wait_canon = false;
			std::vector<AiWaitLoop> ai_wait_loops;
			std::vector<u8> ai_wait_set_sig, ai_wait_yield_sig;
			u32 script_hook = 0;
			int script_file_reg = 5; // a1
			std::vector<ScriptPatchGroup> script_patches;
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
			u32 rng_float = 0, rng_int = 0, rng_float01 = 0, rng_u16 = 0;
			// cosmetic-only draws (particles, wind, blinks, HUD widgets): their own rolled-back stream, so a different
			// cosmetic population (e.g. after an async menu wait) never moves the simulation's RNG
			u32 cosmetic_seed = 0;
			std::unordered_set<u32> cosmetic_sites;
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
		std::vector<u8> ParseHex(const std::string& h)
		{
			std::vector<u8> v;
			for (size_t i = 0; i + 1 < h.size(); i += 2)
				v.push_back(static_cast<u8>(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
			return v;
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
			if (Has(root, "session"))
			{
				static const char* rn[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
					"t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
				const auto se = Child(root, "session");
				if (Has(se, "branches"))
					for (const auto& c : Child(se, "branches").children())
					{
						Manifest::ForcedBranch fb;
						fb.at = Get(c, "at", 0);
						fb.to = Get(c, "to", 0);
						if (Has(c, "set"))
							for (const auto& r : Child(c, "set").children())
							{
								const std::string k(r.key().str, r.key().len);
								for (int i = 1; i < 32; i++)
									if (k == rn[i])
										fb.regs.emplace_back(i, static_cast<u32>(std::strtoul(std::string(r.val().str, r.val().len).c_str(), nullptr, 0)));
							}
						if (fb.at && fb.to)
							m->session_branches.push_back(std::move(fb));
					}
				if (Has(se, "script_patches"))
				{
					const auto sp = Child(se, "script_patches");
					m->script_hook = Get(sp, "hook", 0);
					const std::string fr = GetStr(sp, "file_reg");
					for (int i = 1; i < 32; i++)
						if (fr == rn[i])
							m->script_file_reg = i;
					auto hex = [](const std::string& h) {
						std::vector<u8> v;
						for (size_t i = 0; i + 1 < h.size(); i += 2)
							v.push_back(static_cast<u8>(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
						return v;
					};
					if (Has(sp, "groups"))
						for (const auto& g : Child(sp, "groups").children())
						{
							Manifest::ScriptPatchGroup grp;
							grp.name = GetStr(g, "name");
							for (const auto& c : Child(g, "patches").children())
							{
								Manifest::ScriptPatch pt;
								pt.off = Get(c, "off", 0);
								pt.expect = hex(GetStr(c, "expect"));
								pt.write = hex(GetStr(c, "write"));
								if (pt.expect.size() == pt.write.size() && !pt.write.empty())
									grp.patches.push_back(std::move(pt));
							}
							m->script_patches.push_back(std::move(grp));
						}
				}
				if (Has(se, "link"))
				{
					const auto lk = Child(se, "link");
					auto hook = [&](const char* k, Manifest::LinkHook& h) {
						if (!Has(lk, k))
							return;
						const auto n = Child(lk, k);
						h.at = Get(n, "at", 0);
						const std::string rg = GetStr(n, "reg");
						for (int i = 1; i < 32; i++)
							if (rg == rn[i])
								h.reg = i;
						h.match = GetStr(n, "match");
						h.has_value = Has(n, "value");
						h.value = Get(n, "value", 0);
					};
					auto writes = [&](const char* k, std::vector<Manifest::ExprWrite>& v) {
						if (Has(lk, k))
							for (const auto& c : Child(lk, k).children())
								v.push_back({ParseVal(Child(c, "addr")), ParseVal(Child(c, "value"))});
					};
					m->link.on = true;
					hook("attach", m->link.attach);
					hook("detach", m->link.detach);
					hook("commit", m->link.commit);
					if (Has(lk, "selection"))
						for (const auto& c : Child(lk, "selection").children())
							m->link.selection.emplace_back(Get(c, "addr", 0), static_cast<int>(Get(c, "owner", 0)));
					m->link.seed_addr = Get(lk, "seed", 0);
					writes("attach_writes", m->link.attach_writes);
					writes("commit_writes", m->link.commit_writes);
					if (Has(lk, "hold"))
					{
						const auto hd = Child(lk, "hold");
						m->link.pend_list = Get(hd, "list", 0);
						m->link.pend_pool = Get(hd, "pool", 0);
						m->link.pend_done_bit = Get(hd, "done_bit", 0x100);
						m->link.pend_next_off = Get(hd, "next_off", 8);
						m->link.pend_sentinel = Get(hd, "sentinel", 1);
						if (Has(hd, "jumps"))
							for (const auto& c : Child(hd, "jumps").children())
								{
									m->link.hold_jumps.emplace_back(Get(c, "at", 0), Get(c, "to", 0));
									m->link.hold_wait_to.push_back(Get(c, "wait_to", 0));
								}
						if (Has(hd, "present_skip"))
						{
							m->link.present_at = Get(Child(hd, "present_skip"), "at", 0);
							m->link.present_to = Get(Child(hd, "present_skip"), "to", 0);
						}
						if (Has(hd, "virtual"))
						{
							const auto vl = Child(hd, "virtual");
							auto& V = m->link.vload;
							V.poll_at = Get(vl, "poll_at", 0);
							V.req_reg = static_cast<u32>(Get(vl, "req_reg", 17));
							V.state_off = Get(vl, "state_off", 32);
							V.poll_state = Get(vl, "poll_state", 5);
							V.handle_off = Get(vl, "handle_off", 44);
							V.stat_off = Get(vl, "stat_off", 1);
							V.ready = Get(vl, "ready", 3);
							V.sectors_off = Get(vl, "sectors_off", 52);
							V.base = Get(vl, "base", 2);
							V.sectors_per_tick = std::max<u32>(1, Get(vl, "sectors_per_tick", 64));
							V.stall_to = Get(vl, "stall_to", 0);
						}
					}
					if (Has(lk, "fx_canon"))
					{
						const auto fx = Child(lk, "fx_canon");
						m->fx_canon = Get(fx, "enabled", 1) != 0;
						for (const char* k : {"dlists", "slists"})
							if (Has(fx, k))
								for (const auto& c : Child(fx, k).children())
									(k[0] == 'd' ? m->fx_dlists : m->fx_slists).push_back({Get(c, "head", 0), Get(c, "stride", 0)});
						if (Has(fx, "at"))
							for (const auto& c : Child(fx, "at").children())
								m->fx_at.push_back(ParseU32(c));
						if (Has(fx, "at_writes"))
							for (const auto& c : Child(fx, "at_writes").children())
								m->fx_at_writes.push_back({ParseVal(Child(c, "addr")), ParseVal(Child(c, "value"))});
					}
					if (Has(lk, "ai_wait_canon"))
					{
						const auto aw = Child(lk, "ai_wait_canon");
						m->ai_wait_canon = Get(aw, "enabled", 1) != 0;
						m->ai_wait_set_sig = ParseHex(GetStr(aw, "set_sig"));
						m->ai_wait_yield_sig = ParseHex(GetStr(aw, "yield_sig"));
						if (Has(aw, "loops"))
							for (const auto& c : Child(aw, "loops").children())
								m->ai_wait_loops.push_back({Get(c, "set", 0), Get(c, "yield_pc", 0), Get(c, "reg", 20), Get(c, "value", 600)});
					}
					if (Has(lk, "css_mirror"))
					{
						// async mirror character select (CssMirror.h; FUC notes/CSS_MIRROR_RE.md)
						const auto cm = Child(lk, "css_mirror");
						auto& C = m->css_mirror;
						C.enabled = Get(cm, "enabled", 1) != 0;
						C.tick_hook = Get(cm, "tick_hook", 0);
						C.sig_off = Get(cm, "sig_off", 0);
						C.sig = GetStr(cm, "sig");
						C.scene_addr = Get(cm, "scene_addr", 0);
						C.scene_value = Get(cm, "scene_value", 5);
						auto pair = [&cm](const char* k, std::array<u32, 2>& out) {
							if (Has(cm, k))
							{
								u32 i = 0;
								for (const auto& c : Child(cm, k).children())
									if (i < 2)
										out[i++] = ParseU32(c);
							}
						};
						pair("side_rec", C.side_rec);
						pair("var_col", C.var_col);
						pair("var_row", C.var_row);
						pair("var_chr", C.var_chr);
						pair("var_colour", C.var_colour);
						pair("side_thread", C.side_thread);
						pair("pc_browse", C.pc_browse);
						pair("pc_colour", C.pc_colour);
						pair("pc_locked", C.pc_locked);
						pair("pc_confirm", C.pc_confirm);
						pair("pc_colour_confirm", C.pc_colour_confirm);
						C.var_stage = Get(cm, "var_stage", 0);
						C.var_bgm = Get(cm, "var_bgm", 0);
						C.var_bgm_ok = Get(cm, "var_bgm_ok", 0);
						C.var_diarmuid = Get(cm, "var_grid_b", 0);
						C.var_count_a = Get(cm, "var_colour_count_a", 0);
						C.var_count_b = Get(cm, "var_colour_count_b", 0);
						C.var_stage_avail = Get(cm, "var_stage_avail", 0);
						C.main_thread = Get(cm, "main_thread", 1);
						C.pc_stage = Get(cm, "pc_stage", 0);
						C.stage_id_addr = Get(cm, "stage_id_addr", 0);
						C.confirmed_value = Get(cm, "confirmed_value", 2);
						C.grid_table = Get(cm, "grid_table", 0);
						C.grid_table_b = Get(cm, "grid_table_b", 0);
						C.grid_cols = Get(cm, "grid_cols", 9);
						C.grid_rows = Get(cm, "grid_rows", 2);
						C.random_col = Get(cm, "random_col", 4);
						C.unlock_bits = Get(cm, "unlock_bits", 0);
						C.unlock_base = Get(cm, "unlock_base", 511);
						C.stage_entries = Get(cm, "stage_entries", 10);
						C.bgm_count = Get(cm, "bgm_count", 29);
					}
					if (Has(lk, "onemore"))
					{
						const auto om = Child(lk, "onemore");
						m->link.onemore_decided = ParseVal(Child(om, "decided"));
						m->link.onemore_choice = Get(om, "choice", 0);
						if (Has(om, "vote"))
						{
							const auto vt = Child(om, "vote");
							m->link.vote_sig_off = Get(vt, "sig_off", 0);
							m->link.vote_sig = GetStr(vt, "sig");
							m->link.vote_cursor_var = Get(vt, "cursor_var", 0);
							m->link.vote_retry_index = Get(vt, "retry_index", 0);
							m->link.vote_css_index = Get(vt, "css_index", 1);
						}
						m->link.retry_value = Get(om, "retry_value", 1);
						if (Has(om, "retry_writes"))
							for (const auto& c : Child(om, "retry_writes").children())
								m->link.retry_writes.push_back({ParseVal(Child(c, "addr")), ParseVal(Child(c, "value"))});
						if (Has(om, "css_writes"))
							for (const auto& c : Child(om, "css_writes").children())
								m->link.css_writes.push_back({ParseVal(Child(c, "addr")), ParseVal(Child(c, "value"))});
					}
				}
				if (Has(se, "input_masks"))
					for (const auto& c : Child(se, "input_masks").children())
					{
						Manifest::InputMask im;
						im.player = Get(c, "player", 0);
						im.when = ParseVal(Child(c, "when"));
						im.buttons = static_cast<u16>(Get(c, "buttons", 0));
						m->session_masks.push_back(std::move(im));
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
				m->rng_float01 = Get(r, "float01", 0);
				m->rng_u16 = Get(r, "u16", 0);
				m->cosmetic_seed = Get(r, "cosmetic_seed", 0);
				if (Has(r, "cosmetic_sites"))
					for (const u32 x : ParseList(Child(r, "cosmetic_sites")))
						m->cosmetic_sites.insert(x);
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
			bool attach = false, detach = false, start = false, stop = false, net_start = false, net_stop = false, locks_set = false, locks_on = false;
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
		void RemoveSessionLocks();
		// ---- link mode (async menus, per-battle rollback) ----
		enum class LinkPhase : u8 { Off, Menu, Attaching, Battle, Detaching };
		LinkPhase s_lphase = LinkPhase::Off;
		enum : u16 { MSG_INPUT = 1, MSG_PICK = 2, MSG_CHOICE = 3, MSG_CSS = 4 };
		int s_local = 0;                         // local player (0/1)
		std::deque<std::array<u8, 6>> s_remote_in; // remote player's streamed menu inputs, applied one per read
		std::array<u8, 6> s_remote_last = {0, 0, 0x80, 0x80, 0x80, 0x80};
		std::vector<u32> s_remote_pick;          // the peer's resolved selection values (MSG_PICK)
		bool s_remote_pick_valid = false;
		std::vector<u32> s_commit_sel;           // our committed picks awaiting the peer's for verification
		bool s_wait_hold = false;                // hold whole frames (no sim tick, no render) while waiting for the peer
		s32 s_choice_mine = -1;
		u32 s_attach_frames = 0;                 // frames spent at the attach barrier
		u32 s_wait_frames = 0;                   // held frames while waiting for the peer (diagnostics)
		u32 s_vote_vm = 0;                       // the one-more menu's script VM (latched by signature)
		s32 s_vote_final = -1;                   // agreed one-more result once both votes are in (choice value)
		u16 s_vote_prev = 0;
		s32 s_remote_choice = -1;                // the peer's one-more choice (MSG_CHOICE)
		bool s_attach_req = false, s_detach_req = false, s_onemore = false, s_choice_sent = false;
		// from the battle commit / the agreed one-more choice until the attach: both ports read an identical neutral pad
		// on both peers (the fighters exist and latch input before the attach point; notes/ASYNC_MENUS_ATTACH.md)
		bool s_pre_attach_neutral = false;
		s32 s_menu_frame = 0;
		u32 s_battle_counter = 0;
		std::vector<u32> s_link_hooks;
		std::string s_link_dump_prefix; // desync forensics: EE RAM at every attach (after the canonical writes)
		bool LinkTick();                                // frame boundary: true = this frame runs without netcode
		bool LinkPadRead(u32 player, u8* buf);          // menu phases: stream/feed; true = handled
		void VlBegin();                                 // virtual load clock: open the pre-attach window
		void LinkVerifyPicks();                         // mirror menus: our committed picks vs the peer's
		void OneMoreOnTick(u32 vm);                     // latch the one-more menu script VM
		void AiWaitRecordVm(u32 vm, u32 file);         // RETRY canon: remember AI script VMs (Seq_Init)
		struct AiVm
		{
			u32 vm = 0, file = 0;
			std::vector<const Manifest::AiWaitLoop*> loops;
		};
		std::vector<AiVm> s_ai_vms; // AI script VMs started since the last commit (recorded at Seq_Init)
		void InstallLinkHooks();
		void RemoveLinkHooks();
		void DoStop();
		void DoStart(int mode, u32 frames);
		NetBridge::Plan s_net_plan;
		std::vector<s32> s_net_resim_saves; // save index per re-simulated step
		s32 s_net_fwd_save = -1;
		s32 s_net_frame_base = 0;           // host frame - netcode frame (playback sources start at their own frame 0)
		bool s_net_base_set = false;            // the forward frame's save, answered at the next frame boundary
		u8 s_live_report[PAD_PLAYERS][PAD_REPORT] = {};
		bool s_live_valid[PAD_PLAYERS] = {};
		std::vector<std::pair<u32, u32>> s_hash_ranges; // the manifest's watched (gameplay) state
		u64 s_net_refused = 0;
		// Desync forensics: EE RAM written at chosen netcode frames (the state after frame F, before F+1 runs, i.e. after
		// this tick's corrections). Requested with a journal/replay path suffix ";dump=F1,F2,..." (next to the journal).
		u32 HashState();
		std::vector<s32> s_dump_frames;
		std::string s_dump_prefix;
		void DumpBeforeCurrentFrame()
		{
			if (!s_net || s_dump_frames.empty())
				return;
			const s32 f = s_host_frame - s_net_frame_base - 1;
			if (std::find(s_dump_frames.begin(), s_dump_frames.end(), f) == s_dump_frames.end())
				return;
			const std::string path = fmt::format("{}.f{}.ee", s_dump_prefix, f);
			if (std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "wb"))
			{
				std::fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, fp);
				std::fclose(fp);
				Console.WriteLn("GameRollback: EE RAM after netcode frame %d -> %s (state hash %08X)", f, path.c_str(), HashState());
			}
		}

		// the cross-peer checksum of the watched state: 8 bytes per step (multiply-rotate mix), ~8x the byte-wise FNV it
		// replaces -- it runs on every save, forward and re-simulated. Both peers run the same build.
		u32 HashState()
		{
			u64 h = 0x9E3779B97F4A7C15ull;
			for (const auto& [a, l] : s_hash_ranges)
			{
				const u8* p = &eeMem->Main[a & RAM_MASK];
				u32 k = 0;
				for (; k + 8 <= l; k += 8)
				{
					u64 w;
					std::memcpy(&w, p + k, 8);
					h = (h ^ w) * 0xFF51AFD7ED558CCDull;
					h = (h << 31) | (h >> 33);
				}
				for (; k < l; k++)
					h = (h ^ p[k]) * 1099511628211ull;
				h ^= l; // range lengths are part of the state's shape
			}
			h ^= h >> 33;
			h *= 0xC4CEB9FE1A85EC53ull;
			h ^= h >> 29;
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
				DumpBeforeCurrentFrame();
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
			// P2P pacing: the netcode's suggested frame-time stretch when this peer runs ahead
			const float pace = NetBridge::PaceFactor();
			if (pace > 1.001f)
				std::this_thread::sleep_for(std::chrono::microseconds(static_cast<s64>((std::min(pace, 1.5f) - 1.0f) * 16683.0f)));
			int n = 0;
			for (int waited = 0;; waited++)
			{
				n = NetBridge::Frame(&s_net_plan);
				if (n != 0)
					break;
				if (s_lphase == LinkPhase::Detaching)
				{
					// held at the stop frame: the detach completes here (never run a game frame on a hold)
					const int d = NetBridge::Detach();
					if (d != 0)
					{
						DoStop();
						s_net = false;
						s_detach_req = false;
						s_onemore = true;
						s_choice_sent = false;
						s_vote_final = -1;
						s_vote_prev = 0;
						s_remote_in.clear();
						s_lphase = LinkPhase::Menu;
						return false;
					}
				}
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
				if (s_lphase != LinkPhase::Off)
				{
					// link mode: only this battle's session is lost; the link (menus, next attach) stays up
					while (NetBridge::Attached() && NetBridge::Detach() == 0)
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					DoStop();
					s_net = false;
					s_lphase = LinkPhase::Menu;
					return false;
				}
				NetBridge::Stop();
				s_net = false;
				RemoveSessionLocks();
				return false;
			}
			for (const s32 id : s_net_plan.pre_saves) // the session's frame-0 save: the state right now
				NetBridge::ResolveSave(id, HashState());
			s_net_resim_saves.clear();
			if (!s_net_base_set)
			{
				for (const NetBridge::Step& st : s_net_plan.steps)
					if (!st.rolling_back)
					{
						s_net_frame_base = s_host_frame + 1 - st.frame;
						s_net_base_set = true;
						if (s_net_frame_base != 0)
							Console.WriteLn("GameRollback: netcode frame %d = host frame %d", st.frame, s_host_frame + 1);
						break;
					}
			}
			for (const NetBridge::Step& st0 : s_net_plan.steps)
			{
				NetBridge::Step st = st0;
				st.frame += s_net_frame_base;
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
			PcInput::Poll(); // creamybinder: this frame's local input, before netcode and the game read it
			ApplyRequests();
			if (s_lphase != LinkPhase::Off && LinkTick())
				return EeHooks::Action::Continue; // menus: the frame runs locally, no netcode
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
				Console.Error("GameRollback: UNCORRECTABLE misprediction: netcode asked for a %u-frame rollback at frame %d, device "
							  "did %u (async I/O inside the window): this peer will desync",
					s_net_plan.rollback_advances, s_host_frame, R);
			}
			if (R == 0)
			{
				RollbackDevice::HandleSyscall(RollbackDevice::CMD_CUR_PRE, 0, 0, 0);
				DumpBeforeCurrentFrame();
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
		// Session lock-down (manifest `session`, netplay only): a forced branch is taken exactly like the original branch
		// would be (target + its delay-slot register effects); installed identically on every peer.
		std::vector<u32> s_session_hooks;
		std::map<std::string, u32> s_script_patch_count;
		bool ScriptPatchLogSuppressed(const std::string& name) { return s_script_patch_count[name]++ >= 4; } // log the first few
		// load-time script patches on one script image (Seq_Init), or on a script that was already running when the session
		// started (seen from the VM tick hook): a group applies only while all its expected bytes match, i.e. once
		void ApplyScriptPatches(u32 file)
		{
			if (!file)
				return;
			for (const auto& grp : s_man.script_patches)
			{
				bool all = !grp.patches.empty();
				for (const auto& pt : grp.patches)
				{
					const u32 a = file + pt.off;
					if ((a & RAM_MASK) + pt.expect.size() > Ps2MemSize::MainRam ||
						std::memcmp(&eeMem->Main[a & RAM_MASK], pt.expect.data(), pt.expect.size()) != 0)
					{
						all = false;
						break;
					}
				}
				if (!all)
					continue;
				for (const auto& pt : grp.patches)
					std::memcpy(&eeMem->Main[(file + pt.off) & RAM_MASK], pt.write.data(), pt.write.size());
				if (!ScriptPatchLogSuppressed(grp.name))
					Console.WriteLn("GameRollback: script patch '%s' applied at %08X", grp.name.c_str(), file);
			}
		}
		void InstallSessionLocks()
		{
			for (const auto& fb : s_man.session_branches)
			{
				const u32 to = fb.to;
				const auto regs = fb.regs;
				EeHooks::AddCall(fb.at, [to, regs](u32) {
					for (const auto& [r, v] : regs)
						cpuRegs.GPR.r[r].UD[0] = static_cast<u64>(static_cast<s64>(static_cast<s32>(v)));
					cpuRegs.pc = to;
					return EeHooks::Action::Jump;
				}, EeHooks::OWNER_GAME);
				s_session_hooks.push_back(fb.at);
			}
			if (s_man.script_hook && (!s_man.script_patches.empty() || s_man.ai_wait_canon))
			{
				EeHooks::AddCall(s_man.script_hook, [](u32) {
					const u32 file = cpuRegs.GPR.r[s_man.script_file_reg].UL[0];
					AiWaitRecordVm(cpuRegs.GPR.n.a0.UL[0], file);
					ApplyScriptPatches(file);
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
				s_session_hooks.push_back(s_man.script_hook);
			}
			if (!s_man.session_branches.empty() || !s_man.session_masks.empty())
				Console.WriteLn("GameRollback: session locks on (%zu branches, %zu input masks)", s_man.session_branches.size(),
					s_man.session_masks.size());
		}
		void RemoveSessionLocks()
		{
			for (const u32 a : s_session_hooks)
				EeHooks::Remove(a);
			s_session_hooks.clear();
		}
		// Input masks are evaluated on the state the read happens in (forward and re-simulated alike), so they are part of
		// the deterministic simulation: the released bits are forced in the report the game gets.
		void ApplySessionMasks(u32 player, u8* buf)
		{
			for (const auto& im : s_man.session_masks)
			{
				if (im.player != player)
					continue;
				s64 v = 0;
				if (!Eval(im.when, 0, &v) || v == 0)
					continue;
				buf[2] |= static_cast<u8>(im.buttons >> 8); // active-low: set = released
				buf[3] |= static_cast<u8>(im.buttons & 0xFF);
				if ((buf[1] & 0x0F) >= 9)
				{
					static constexpr u16 PRESS_BITS[12] = {0x2000, 0x8000, 0x1000, 0x4000, 0x10, 0x20, 0x40, 0x80, 0x4, 0x8, 0x1, 0x2};
					for (u32 i = 0; i < 12; i++)
						if (im.buttons & PRESS_BITS[i])
							buf[8 + i] = 0;
				}
			}
		}

		EeHooks::Action OnPadReadReturn(u32)
		{
			if (!s_pad_pending)
				return EeHooks::Action::Continue;
			s_pad_pending = false;
			const u32 player = s_pad_player % PAD_PLAYERS;
			u8* buf = &eeMem->Main[s_pad_buf & RAM_MASK];
			if (s_lphase != LinkPhase::Off && s_lphase != LinkPhase::Battle && s_lphase != LinkPhase::Detaching)
			{
				cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(RollbackDevice::HandleSyscall(
					RollbackDevice::CMD_PAD_FEED, s_pad_player, s_pad_buf, cpuRegs.GPR.n.v0.UL[0]));
				if (LinkPadRead(player, buf))
					cpuRegs.GPR.n.v0.SD[0] = 1;
				return EeHooks::Action::Continue;
			}
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
				if (s_net)
					ApplySessionMasks(player, buf);
				return EeHooks::Action::Continue;
			}
			cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(RollbackDevice::HandleSyscall(
				RollbackDevice::CMD_PAD_FEED, s_pad_player, s_pad_buf, cpuRegs.GPR.n.v0.UL[0]));
			if (PcInput::Active() && !s_net && player < 2)
			{
				// offline PovertyCaster session: both seats are local creamybinder players
				const u16 b = PcInput::Buttons(static_cast<int>(player));
				const u8 in[6] = {static_cast<u8>(b >> 8), static_cast<u8>(b), 0x80, 0x80, 0x80, 0x80};
				BuildReport(in, buf);
				cpuRegs.GPR.n.v0.SD[0] = 1;
			}
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
				ApplySessionMasks(player, buf);
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

		// the game's LCG (FUC Rand_*: seed = seed*214013 + 2531011) on a separate seed word, every variant exact
		void RngOnSeed(u32 pc, u32 seed_addr)
		{
			const u32 seed = Rd(seed_addr) * 214013u + 2531011u;
			Wr(seed_addr, seed);
			const u32 hi = seed >> 16;
			if (pc == s_man.rng_float)
			{
				float lo, h;
				std::memcpy(&lo, &fpuRegs.fpr[12].UL, 4);
				std::memcpy(&h, &fpuRegs.fpr[13].UL, 4);
				const float r = lo + (static_cast<float>(hi) * (h - lo)) / 65536.0f;
				std::memcpy(&fpuRegs.fpr[0].UL, &r, 4);
			}
			else if (pc == s_man.rng_float01)
			{
				const float r = static_cast<float>(hi) / 65536.0f;
				std::memcpy(&fpuRegs.fpr[0].UL, &r, 4);
			}
			else if (pc == s_man.rng_u16)
				cpuRegs.GPR.n.v0.SD[0] = static_cast<s64>(hi);
			else
			{
				const s32 lo = cpuRegs.GPR.n.a0.SL[0], h = cpuRegs.GPR.n.a1.SL[0];
				cpuRegs.GPR.n.v0.SD[0] = static_cast<s32>(lo + ((hi * static_cast<u32>(h - lo)) >> 16));
			}
		}
		EeHooks::Action OnRng(u32 pc)
		{
			const u32 ra = cpuRegs.GPR.n.ra.UL[0];
			if (s_man.cosmetic_seed && s_man.cosmetic_sites.count(ra - 8))
			{
				RngOnSeed(pc, s_man.cosmetic_seed);
				return EeHooks::Action::Return;
			}
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
			if (s_man.cosmetic_seed)
				for (const u32 site : s_man.cosmetic_sites)
					callers.push_back(site + 8);
			for (const u32 fn : {s_man.rng_float, s_man.rng_int, s_man.rng_float01, s_man.rng_u16})
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
			if (s_man.cosmetic_seed)
				AddRegion(s_man.cosmetic_seed, 4);
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
			if (s_lphase == LinkPhase::Off) // link mode keeps the simulation hooks (virtual audio clock, RNG splits,
				RemoveRollbackHooks();       // pad feed) through menus: their resim gates are inert outside resims
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

		// ================= link mode runtime =================

		void LinkPump()
		{
			NetBridge::Message m;
			while (NetBridge::LinkPoll(&m))
			{
				if (m.type == MSG_INPUT && m.data.size() == 6)
				{
					std::array<u8, 6> in;
					std::memcpy(in.data(), m.data.data(), 6);
					s_remote_in.push_back(in);
				}
				else if (m.type == MSG_PICK)
				{
					s_remote_pick.assign(m.data.size() / 4, 0);
					std::memcpy(s_remote_pick.data(), m.data.data(), s_remote_pick.size() * 4);
					s_remote_pick_valid = true;
					if (CssMirror::Enabled())
						LinkVerifyPicks();
				}
				else if (m.type == MSG_CHOICE && m.data.size() >= 4)
					std::memcpy(&s_remote_choice, m.data.data(), 4);
				else if (m.type == MSG_CSS)
					CssMirror::OnMessage(m.data.data(), static_cast<u32>(m.data.size()));
			}
		}
		void ApplyWrites(const std::vector<Manifest::ExprWrite>& ws)
		{
			for (const auto& w : ws)
			{
				s64 a = 0, v = 0;
				if (Eval(w.addr, 0, &a) && Eval(w.value, 0, &v) && a != 0)
					Wr(static_cast<u32>(a), static_cast<u32>(v));
			}
		}
		template <typename Pred>
		bool LinkWait(Pred done, const char* what)
		{
			for (int ms = 0; ms < 600000; ms++) // a human decision: up to 10 minutes, the link keeps pumping
			{
				LinkPump();
				if (done())
					return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Console.Error("GameRollback: link: timed out waiting for %s", what);
			return false;
		}
		bool LinkHookMatches(const Manifest::LinkHook& h)
		{
			if (h.has_value)
				return cpuRegs.GPR.r[h.reg].UL[0] == h.value;
			const u32 p = cpuRegs.GPR.r[h.reg].UL[0] & RAM_MASK;
			return !h.match.empty() && p + h.match.size() < Ps2MemSize::MainRam &&
				   std::memcmp(&eeMem->Main[p], h.match.data(), h.match.size()) == 0;
		}
		// Selection commit (e.g. FUC Battle_InitPhase(1)): send our resolved picks, wait for the peer's, write the
		// agreed record so the battle never depends on local menu timing. EE thread, inside the hook.
		// mirror menus: compare our committed picks with the peer's once both exist (diagnostic; the attach id enforces it)
		void LinkVerifyPicks()
		{
			if (s_commit_sel.empty() || !s_remote_pick_valid)
				return;
			bool same = s_remote_pick.size() >= s_commit_sel.size();
			for (size_t i = 0; same && i < s_commit_sel.size(); i++)
				same = s_remote_pick[i] == s_commit_sel[i];
			if (same)
				Console.WriteLn("GameRollback: link: picks agree with the peer");
			else
				Console.Error("GameRollback: link: picks DIFFER from the peer's (the attach will refuse this battle)");
			s_commit_sel.clear();
			s_remote_pick_valid = false;
		}
		void LinkCommit()
		{
			const auto& sel = s_man.link.selection;
			std::vector<u32> mine(sel.size() + 1, 0);
			for (size_t i = 0; i < sel.size(); i++)
				mine[i] = Rd(sel[i].first);
			mine[sel.size()] = s_man.link.seed_addr ? Rd(s_man.link.seed_addr) : 0;
			NetBridge::LinkSend(MSG_PICK, s_menu_frame, mine.data(), static_cast<u32>(mine.size() * 4));
			if (CssMirror::Enabled())
			{
				// mirror menus: both PCs already hold the agreed picks (each side's owner drove it on both), so nothing
				// is awaited here -- the picks travel for verification only (the attach id hashes them, so a mismatch
				// can never start a wrong battle). The seed comes from what both PCs share: the picks and the battle
				// number (P1's menu RNG differs between the PCs and would have to be waited for).
				u64 h = 1469598103934665603ull ^ (0x5EEDull + s_battle_counter);
				for (size_t i = 0; i < sel.size(); i++)
					h = (h ^ mine[i]) * 1099511628211ull;
				mine[sel.size()] = static_cast<u32>(h ^ (h >> 32));
				if (s_man.link.seed_addr)
					Wr(s_man.link.seed_addr, mine[sel.size()]);
				s_commit_sel.assign(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(sel.size()));
				LinkVerifyPicks();
			}
			else
			{
				if (!LinkWait([] { return s_remote_pick_valid; }, "the peer's picks"))
					return;
				for (size_t i = 0; i < sel.size() && i < s_remote_pick.size(); i++)
					if (sel[i].second != s_local)
						Wr(sel[i].first, s_remote_pick[i]);
				if (s_man.link.seed_addr)
					Wr(s_man.link.seed_addr, s_local == 0 ? mine[sel.size()] : s_remote_pick[sel.size()]);
				s_remote_pick_valid = false;
			}
			s_pre_attach_neutral = true;
			s_ai_vms.clear(); // a new battle loads new AI scripts
			VlBegin();
			// the commit -> attach window must run identically: reset what the async menus let drift (the virtual
			// audio clock restarts from its canonical empty state, like at every rollback start)
			ApplyWrites(s_man.link.commit_writes);
			if (s_man.vs.state)
				VsInit();
			Console.WriteLn("GameRollback: link: selections committed (%zu values)", sel.size());
		}
		// the game's pending load requests (FUC LoadReq_CountPending: list nodes -> request, done bit)
		u32 LinkPendingLoads()
		{
			const auto& L = s_man.link;
			if (!L.pend_list || (L.pend_pool && Rd(L.pend_pool) == 0))
				return 0;
			u32 n = 0, node = Rd(L.pend_list);
			for (int guard = 0; guard < 4096 && node; guard++)
			{
				const u32 req = Rd(node);
				if (req == L.pend_sentinel)
					break;
				if ((Rd(req) & L.pend_done_bit) == 0)
					n++;
				node = Rd(node + L.pend_next_off);
			}
			return n;
		}
		// ---- RETRY re-attach canonicalization (FUC notes/RETRY_CANON.md) ----
		void AiWaitRecordVm(u32 vm, u32 file)
		{
			const auto& L = s_man;
			if (!L.ai_wait_canon || !vm || !file || L.ai_wait_set_sig.empty() || L.ai_wait_yield_sig.empty())
				return;
			AiVm rec{vm, file, {}};
			for (const auto& lp : L.ai_wait_loops)
			{
				const u32 a = (file + lp.set) & RAM_MASK, b = (file + lp.yield_pc) & RAM_MASK;
				if (a + L.ai_wait_set_sig.size() <= Ps2MemSize::MainRam && b + L.ai_wait_yield_sig.size() <= Ps2MemSize::MainRam &&
					std::memcmp(&eeMem->Main[a], L.ai_wait_set_sig.data(), L.ai_wait_set_sig.size()) == 0 &&
					std::memcmp(&eeMem->Main[b], L.ai_wait_yield_sig.data(), L.ai_wait_yield_sig.size()) == 0)
					rec.loops.push_back(&lp);
			}
			if (rec.loops.empty())
				return;
			s_ai_vms.erase(std::remove_if(s_ai_vms.begin(), s_ai_vms.end(), [vm](const AiVm& v) { return v.vm == vm; }), s_ai_vms.end());
			s_ai_vms.push_back(std::move(rec));
		}
		// every thread of a recorded AI VM parked in a "wait for FIGHT or N frames" loop restarts its countdown now
		void AiWaitCanon()
		{
			if (!s_man.ai_wait_canon)
				return;
			u32 n_written = 0;
			for (const AiVm& v : s_ai_vms)
			{
				const u32 ctx = Rd(v.vm + 12), base = Rd(v.vm + 28), n = Rd(v.vm + 4) >> 16; // u16 at vm+6
				if (!ctx || base != v.file)
					continue; // the VM ended or was reused
				for (u32 t = 1; t < n && t < 64; t++)
				{
					const u32 pc = Rd(ctx + 96 * t + 88);
					for (const auto* lp : v.loops)
						if (pc && pc - base == lp->yield_pc)
						{
							Wr(ctx + 96 * t + 4 * lp->reg, lp->value);
							n_written++;
						}
				}
			}
			Console.WriteLn("GameRollback: ai_wait_canon: %u countdown(s) restarted (%zu AI VMs)", n_written, s_ai_vms.size());
		}
		// effect pools: free nodes relinked in pool-init order and zeroed; live nodes untouched
		void FxCanon()
		{
			const auto& L = s_man;
			if (!L.fx_canon)
				return;
			std::string log;
			for (const auto& fl : L.fx_dlists)
			{
				// {+0 = 1 (head marker), +4 last, +8 first}; node {+0 self, +4 prev, +8 next}; FIFO, ascending at init
				std::vector<u32> nodes;
				for (u32 n = Rd(fl.head + 8), guard = 0; n && n != fl.head && guard < 65536; n = Rd(n + 8), guard++)
					nodes.push_back(n);
				std::sort(nodes.begin(), nodes.end());
				for (size_t i = 0; i < nodes.size(); i++)
				{
					std::memset(&eeMem->Main[nodes[i] & RAM_MASK], 0, fl.stride);
					Wr(nodes[i] + 0, nodes[i]);
					Wr(nodes[i] + 4, i ? nodes[i - 1] : fl.head);
					Wr(nodes[i] + 8, i + 1 < nodes.size() ? nodes[i + 1] : fl.head);
				}
				Wr(fl.head + 0, 1);
				Wr(fl.head + 4, nodes.empty() ? fl.head : nodes.back());
				Wr(fl.head + 8, nodes.empty() ? fl.head : nodes.front());
				log += fmt::format(" d{:x}:{}", fl.head, nodes.size());
			}
			for (const auto& fl : L.fx_slists)
			{
				// singly linked LIFO {+0 next}; built by pushing ascending addresses, so the top is the highest
				std::vector<u32> nodes;
				for (u32 n = Rd(fl.head), guard = 0; n && guard < 65536; n = Rd(n), guard++)
					nodes.push_back(n);
				std::sort(nodes.begin(), nodes.end());
				u32 top = 0;
				for (const u32 n : nodes)
				{
					std::memset(&eeMem->Main[n & RAM_MASK], 0, fl.stride);
					Wr(n, top);
					top = n;
				}
				Wr(fl.head, top);
				log += fmt::format(" s{:x}:{}", fl.head, nodes.size());
			}
			Console.WriteLn("GameRollback: fx_canon: free lists relinked:%s", log.c_str());
		}

		bool s_hold_frame = false; // decided at the first hold jump of the frame, reused by the others
		u64 s_held_frames = 0;

		// ---- virtual load clock (manifest link.hold.virtual) ----
		struct VlReq
		{
			u32 first = 0, deadline = 0, sectors = 0, last = 0; // last = tick of the latest poll
			s32 real_ready = -1; // loader ticks from the first poll until the real read reported ready
		};
		std::unordered_map<u32, VlReq> s_vl_reqs;  // reads in flight, by request slot (the pool recycles slots)
		std::vector<VlReq> s_vl_done;              // finished reads this window (report)
		std::vector<u32> s_vl_pre;  // requests already in flight when the window opened: they finish on real time
		u32 s_vl_tick = 0;          // loader ticks run in the window (= sim ticks: one each per unheld frame)
		u32 s_vl_frame_tick = 0;    // this frame's tick, as the loader's poll sees it
		u64 s_vl_stalls = 0, s_vl_drain = 0, s_vl_late = 0;
		u32 s_hold_to = 0;          // this frame's first hold jump target
		bool VlOn() { return s_man.link.vload.poll_at != 0 && s_pre_attach_neutral; }
		template <typename F>
		void ForEachLoadReq(F f)
		{
			const auto& L = s_man.link;
			if (!L.pend_list || (L.pend_pool && Rd(L.pend_pool) == 0))
				return;
			u32 node = Rd(L.pend_list);
			for (int guard = 0; guard < 4096 && node; guard++)
			{
				const u32 req = Rd(node);
				if (req == L.pend_sentinel)
					break;
				f(req);
				node = Rd(node + L.pend_next_off);
			}
		}
		bool VlRealReady(u32 req)
		{
			const auto& V = s_man.link.vload;
			const u32 h = Rd(req + V.handle_off);
			return h != 0 && static_cast<u32>(static_cast<s8>(eeMem->Main[(h + V.stat_off) & RAM_MASK])) == V.ready;
		}
		// the commit / agreed RETRY opens the window: the loader's clock starts at 0 on both peers
		void VlBegin()
		{
			s_vl_reqs.clear();
			s_vl_done.clear();
			s_vl_pre.clear();
			s_vl_tick = s_vl_frame_tick = 0;
			s_vl_stalls = s_vl_drain = s_vl_late = 0;
			ForEachLoadReq([](u32 req) {
				if ((Rd(req) & s_man.link.pend_done_bit) == 0)
					s_vl_pre.push_back(req);
			});
		}
		void VlReport()
		{
			if (!s_man.link.vload.poll_at)
				return;
			u32 sectors = 0;
			s32 worst = -1;
			for (const auto& r : s_vl_done)
			{
				sectors += r.sectors;
				worst = std::max(worst, r.real_ready);
				Console.WriteLn("GameRollback: vload: read at tick %u, %u sectors: virtual %u ticks, real %d", r.first, r.sectors,
					r.deadline - r.first, r.real_ready);
			}
			if (!s_vl_reqs.empty())
				Console.Error("GameRollback: vload: %zu reads still in flight at attach", s_vl_reqs.size());
			Console.WriteLn("GameRollback: vload: %zu reads, %u sectors, %u ticks; worst real %d ticks; stall frames %llu, "
							"drain frames %llu (in flight at the commit: %zu), late polls %llu",
				s_vl_done.size(), sectors, s_vl_tick, worst, static_cast<unsigned long long>(s_vl_stalls),
				static_cast<unsigned long long>(s_vl_drain), s_vl_pre.size(), static_cast<unsigned long long>(s_vl_late));
		}
		// frame start (first hold jump): hold or run this frame
		bool VlDecide(u32 normal_to)
		{
			const auto& V = s_man.link.vload;
			bool pre = false;
			ForEachLoadReq([&pre](u32 req) {
				if ((Rd(req) & s_man.link.pend_done_bit) == 0 &&
					std::find(s_vl_pre.begin(), s_vl_pre.end(), req) != s_vl_pre.end())
					pre = true;
			});
			if (pre)
			{
				// a read started before the window: let it finish with the sim held (loader running), as option B did
				s_vl_drain++;
				s_hold_to = normal_to;
				return true;
			}
			bool stall = false;
			ForEachLoadReq([&stall, &V](u32 req) {
				if (Rd(req + V.state_off) != V.poll_state)
					return;
				const auto it = s_vl_reqs.find(req);
				if (it != s_vl_reqs.end() && s_vl_tick >= it->second.deadline && !VlRealReady(req))
				{
					stall = true;
					if (s_vl_stalls < 4 || (s_vl_stalls % 30) == 0) // diagnostics: which read, what the ADXF status says
					{
						const u32 h = Rd(req + V.handle_off);
						Console.WriteLn("GameRollback: vload: stall %llu: req %08x state %u handle %08x stat %d tick %u deadline %u",
							static_cast<unsigned long long>(s_vl_stalls), req, Rd(req + V.state_off), h,
							h ? static_cast<int>(static_cast<s8>(eeMem->Main[(h + V.stat_off) & RAM_MASK])) : -99, s_vl_tick,
							it->second.deadline);
					}
				}
			});
			if (stall)
			{
				// the virtual read is due and the disc isn't done: hold sim AND loader (no tick passes)
				s_vl_stalls++;
				s_hold_to = V.stall_to ? V.stall_to : normal_to;
				return true;
			}
			s_vl_frame_tick = s_vl_tick++;
			return false;
		}
		void InstallLinkHooks()
		{
			const auto& L = s_man.link;
			for (size_t k = 0; k < L.hold_jumps.size(); k++)
			{
				const auto [at, to] = L.hold_jumps[k];
				const u32 wait_to = k < L.hold_wait_to.size() ? L.hold_wait_to[k] : 0;
				const bool first = (k == 0);
				EeHooks::AddCall(at, [to, wait_to, first](u32) {
					if (first)
					{
						s_hold_to = to;
						if (s_wait_hold)
							s_hold_frame = true; // waiting for the peer (one-more choice, attach barrier): a still frame
						else if (VlOn())
							s_hold_frame = VlDecide(to);
						else
							s_hold_frame = s_pre_attach_neutral && LinkPendingLoads() > 0;
						s_held_frames += (s_hold_frame && !s_wait_hold) ? 1 : 0; // load holds only
					}
					if (!s_hold_frame)
						return EeHooks::Action::Continue;
					// waiting for the peer: the scene is still drawn (a held frame that skips drawing re-presents a stale back
					// buffer -> flicker); load holds skip drawing, the pre-attach window must stay identical
					const u32 target = (s_wait_hold && wait_to) ? wait_to : (first ? s_hold_to : to);
					if (s_wait_hold && first)
						s_wait_frames++;
					if (target == cpuRegs.pc)
						return EeHooks::Action::Continue;
					cpuRegs.pc = target;
					return EeHooks::Action::Jump;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(at);
			}
			if (L.vload.poll_at)
			{
				// the loader's "read finished?" poll: answer from the virtual clock (never earlier than the real disc)
				EeHooks::AddCall(L.vload.poll_at, [](u32) {
					if (!VlOn())
						return EeHooks::Action::Continue;
					const auto& V = s_man.link.vload;
					const u32 req = cpuRegs.GPR.r[V.req_reg].UL[0];
					const bool real = cpuRegs.GPR.r[2].UL[0] != 0;
					if (std::find(s_vl_pre.begin(), s_vl_pre.end(), req) != s_vl_pre.end())
						return EeHooks::Action::Continue; // started before the window: real time (drained)
					auto it = s_vl_reqs.find(req);
					// a slot polled every unheld tick while its read runs: a gap means an earlier read in this slot ended
					// without our answer (error path) and this is a new read
					if (it != s_vl_reqs.end() && !s_hold_frame && s_vl_frame_tick > it->second.last + 1)
					{
						s_vl_reqs.erase(it);
						it = s_vl_reqs.end();
					}
					if (it == s_vl_reqs.end())
					{
						VlReq r;
						r.first = s_vl_frame_tick;
						r.sectors = Rd(req + V.sectors_off);
						r.last = r.first;
						r.deadline = r.first + std::max<u32>(1, V.base) + (r.sectors + V.sectors_per_tick - 1) / V.sectors_per_tick;
						it = s_vl_reqs.emplace(req, r).first;
					}
					VlReq& r = it->second;
					if (!s_hold_frame)
						r.last = s_vl_frame_tick;
					if (real && r.real_ready < 0)
						r.real_ready = static_cast<s32>(s_vl_frame_tick - r.first);
					const bool due = !s_hold_frame && s_vl_frame_tick >= r.deadline;
					if (due && !real)
					{
						s_vl_late++; // the frame-start stall check should have held this frame
						const u32 h = Rd(req + V.handle_off);
						Console.WriteLn("GameRollback: vload: late poll: req %08x handle %08x (a0 %08x) stat %d tick %u deadline %u", req, h,
							cpuRegs.GPR.r[4].UL[0], h ? static_cast<int>(static_cast<s8>(eeMem->Main[(h + V.stat_off) & RAM_MASK])) : -99,
							s_vl_frame_tick, r.deadline);
					}
					cpuRegs.GPR.r[2].UD[0] = (due && real) ? 1 : 0;
					if (due && real)
					{
						// this read is finished for the game (state 6 next): the slot's next read starts a new entry
						s_vl_done.push_back(r);
						s_vl_reqs.erase(it);
					}
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(L.vload.poll_at);
			}
			for (const u32 pc : s_man.fx_at)
			{
				// the RETRY kill left every effect pool empty: canonical order now, before the stage re-creates its
				// emitters and they draw particles, so every allocation up to the attach lands on the same addresses
				EeHooks::AddCall(pc, [](u32) {
					if (s_pre_attach_neutral && s_lphase == LinkPhase::Menu)
					{
						FxCanon();
						ApplyWrites(s_man.fx_at_writes);
					}
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(pc);
			}
			if (L.present_at && L.present_to)
			{
				EeHooks::AddCall(L.present_at, [](u32) {
					if (!s_hold_frame)
						return EeHooks::Action::Continue;
					cpuRegs.pc = s_man.link.present_to; // no flip, no clear; the vblank wait still paces the frame
					return EeHooks::Action::Jump;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(L.present_at);
			}
			if (const u32 pc = CssMirror::TickHookPc())
			{
				EeHooks::AddCall(pc, [](u32) {
					const u32 vm = cpuRegs.GPR.n.a0.UL[0];
					CssMirror::OnScriptTick(vm);
					OneMoreOnTick(vm);
					if (const u32 t0 = Rd(vm + 12))
						ApplyScriptPatches(Rd(t0 + 92)); // scripts that were already running when the session started
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(pc);
			}
			// attach and detach may share one pc (FUC: Seq_DebugPrint with different strings)
			std::map<u32, int> at;
			if (L.attach.at)
				at[L.attach.at] |= 1;
			if (L.detach.at)
				at[L.detach.at] |= 2;
			for (const auto& [pc, which] : at)
			{
				EeHooks::AddCall(pc, [which](u32) {
					if ((which & 1) && LinkHookMatches(s_man.link.attach))
						s_attach_req = true;
					if ((which & 2) && LinkHookMatches(s_man.link.detach))
						s_detach_req = true;
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(pc);
			}
			if (L.commit.at)
			{
				EeHooks::AddCall(L.commit.at, [](u32) {
					if (!RollbackDevice::ResimulatingFlag() || !*RollbackDevice::ResimulatingFlag())
						if (LinkHookMatches(s_man.link.commit))
							LinkCommit();
					return EeHooks::Action::Continue;
				}, EeHooks::OWNER_GAME);
				s_link_hooks.push_back(L.commit.at);
			}
		}
		void RemoveLinkHooks()
		{
			for (const u32 a : s_link_hooks)
				EeHooks::Remove(a);
			s_link_hooks.clear();
		}
		u32 LinkAttachId()
		{
			u64 h = 1469598103934665603ull ^ s_battle_counter;
			for (const auto& [a, owner] : s_man.link.selection)
				h = (h ^ Rd(a)) * 1099511628211ull;
			return static_cast<u32>(h ^ (h >> 32));
		}
		// Frame boundary. Returns true when this frame runs locally (menus / waiting).
		std::string s_desync_prefix;       // desync evidence files: <recording path minus .pcrep>.desync.bN.frameF.*
		u32 s_desync_dumped_battle = ~0u;
		std::string s_last_sync;   // the desync detector's latest verdict (kept for the menus after a battle)
		u32 s_last_sync_battle = 0;
		std::mutex s_badge_mtx;
		std::string s_badge;
		void LinkBadgeUpdate()
		{
			static u32 tick = 0;
			static LinkPhase last = LinkPhase::Off;
			static bool last_wait = false;
			if (s_lphase == last && s_wait_hold == last_wait && ++tick < 30)
				return;
			tick = 0;
			last = s_lphase;
			last_wait = s_wait_hold;
			std::string b;
			if (s_lphase != LinkPhase::Off)
			{
				// line 1: who/where; in battle also the link, the rollback activity and what rollbacks cost
				b = fmt::format("P{}", s_local + 1);
				NetBridge::NetStats ns;
				const bool battle = (s_lphase == LinkPhase::Battle || s_lphase == LinkPhase::Detaching) && NetBridge::GetStats(&ns);
				if (s_wait_hold)
					b += " | waiting for the other player";
				else if (battle)
					b += fmt::format(" | battle {} | frame {}", s_battle_counter, ns.frame);
				else
					b += CssMirror::InCss() ? " | character select" : " | menus";
				if (battle)
				{
					b += fmt::format(" | ping {} ms +-{} | input delay {}f | ahead {:+.1f}f", ns.ping_ms, ns.jitter_ms, ns.delay, ns.frames_ahead);
					// the desync detector: cross-peer checksum compares of confirmed frames
					s_last_sync = ns.desynced ?
									  fmt::format("DESYNC at frame {} (checks {}/{} mismatched)", ns.desync_frame, ns.compares, ns.mismatches) :
									  fmt::format("sync OK | {} checks, {} mismatches", ns.compares, ns.mismatches);
					s_last_sync_battle = s_battle_counter;
					b += "\n" + s_last_sync;
					b += fmt::format("\nrollbacks {} (last {}f, {} frames resimulated) | stalls {}", ns.rollbacks,
						ns.last_rollback_frames, ns.rollback_frames_total, ns.stalled);
					RollbackDevice::Perf pf;
					if (RollbackDevice::GetPerf(&pf))
						b += fmt::format("\nrollback cost avg {:.2f} ms (load {:.2f}, resim {:.2f}) max {:.1f} | sim {:.2f} ms/f | "
										 "frame {:.1f} ms p99 {} ({:.1f}% late)",
							pf.avg_us / 1000.0, pf.avg_load_us / 1000.0, pf.avg_resim_us / 1000.0, pf.max_us / 1000.0, pf.sim_us / 1000.0,
							pf.pace_avg_ms, pf.pace_p99_ms, pf.late_pct);
				}
				else
				{
					if (const u32 ping = NetBridge::LinkPingMs())
						b += fmt::format(" | ping {} ms", ping);
					b += fmt::format(" | input delay {}f", NetBridge::InputDelay());
					if (s_last_sync_battle)
						b += fmt::format("\nlast battle {}: {}", s_last_sync_battle,
							s_last_sync.rfind("DESYNC", 0) == 0 ? s_last_sync : "in sync (" + s_last_sync.substr(10) + ")");
				}
			}
			std::lock_guard lk(s_badge_mtx);
			s_badge = std::move(b);
		}
		void LinkWaitOsd()
		{
			static bool shown = false;
			static u32 refresh = 0;
			if (s_wait_hold)
			{
				if (!shown || ++refresh >= 30)
				{
					Host::AddKeyedOSDMessage("pcrb_link_wait", "Waiting for the other player...", 1.0f);
					refresh = 0;
				}
				shown = true;
			}
			else if (shown)
			{
				Host::RemoveKeyedOSDMessage("pcrb_link_wait");
				shown = false;
			}
		}
		bool LinkTick()
		{
			LinkPump();
			LinkWaitOsd();
			LinkBadgeUpdate();
			switch (s_lphase)
			{
				case LinkPhase::Menu:
				{
					s_menu_frame++;
					CssMirror::Frame(s_menu_frame, [](const void* d, u32 n) { NetBridge::LinkSend(MSG_CSS, s_menu_frame, d, n); });
					if (s_onemore)
					{
						s64 d = 0;
						const bool decided = Eval(s_man.link.onemore_decided, 0, &d) && d != 0;
						if (decided && !s_choice_sent)
						{
							// the game confirmed before a vote was taken (vote support off / menu not latched): send it now and hold
							// the game's frames until the peer's choice arrives
							s_choice_mine = static_cast<s32>(Rd(s_man.link.onemore_choice));
							NetBridge::LinkSend(MSG_CHOICE, s_menu_frame, &s_choice_mine, 4);
							s_choice_sent = true;
						}
						if (!decided && s_choice_sent && s_remote_choice >= 0 && s_vote_final < 0)
						{
							// both votes in: RETRY iff both RETRY; the pad path now drives the menu there and confirms
							const s32 rv = static_cast<s32>(s_man.link.retry_value);
							s_vote_final = (s_choice_mine == rv && s_remote_choice == rv) ? rv : (rv ? 0 : 1);
						}
						s_wait_hold = decided && s_remote_choice < 0;
						if (decided && s_remote_choice >= 0)
						{
							// both chose: both apply the agreed result (RETRY iff both RETRY)
							const s32 mine = s_choice_mine;
							const bool retry = mine == static_cast<s32>(s_man.link.retry_value) &&
											   s_remote_choice == static_cast<s32>(s_man.link.retry_value);
							ApplyWrites(retry ? s_man.link.retry_writes : s_man.link.css_writes);
							s_pre_attach_neutral = retry; // RETRY: straight to the attach point with neutral pads
							if (retry)
							{
								VlBegin();
								ApplyWrites(s_man.link.commit_writes); // the RETRY -> attach window, like a commit
								if (s_man.vs.state)
									VsInit();
							}
							Console.WriteLn("GameRollback: link: one-more %s (mine %d, peer %d; %s, held %u frames)",
								retry ? "RETRY" : "CHARACTER SELECT", mine, s_remote_choice, s_vote_final >= 0 ? "voted" : "confirmed first",
								s_wait_frames);
							s_wait_frames = 0;
							s_remote_choice = -1;
							s_vote_final = -1;
							s_onemore = false;
						}
						if (s_wait_hold)
							return true;
					}
					if (!s_attach_req)
						return true;
					s_attach_req = false;
					s_lphase = LinkPhase::Attaching;
					[[fallthrough]];
				}
				case LinkPhase::Attaching:
				{
					// the barrier is polled once per frame; until both PCs are in, the game's frames are held (no sim
					// tick, no render) so the attach frame stays exactly where the attach point left it
					const u32 id = LinkAttachId();
					const int r = NetBridge::Attach(id);
					if (r != 1)
					{
						if (r < 0 || ++s_attach_frames > 1200)
						{
							Console.Error("GameRollback: link: attach failed (%d after %u frames): battle runs locally", r, s_attach_frames);
							s_wait_hold = false;
							s_attach_frames = 0;
							s_lphase = LinkPhase::Menu;
							return true;
						}
						s_wait_hold = true;
						return true;
					}
					if (s_attach_frames)
						Console.WriteLn("GameRollback: link: attach barrier held %u frames", s_attach_frames);
					s_wait_hold = false;
					s_attach_frames = 0;
					VlReport();
					s_pre_attach_neutral = false;
					ApplyWrites(s_man.link.attach_writes); // canonical state at every (re-)attach
					FxCanon();
					AiWaitCanon();
					if (!s_link_dump_prefix.empty())
					{
						const std::string out = fmt::format("{}.gen{}", s_link_dump_prefix, s_battle_counter + 1);
						if (std::FILE* fp = FileSystem::OpenCFile(out.c_str(), "wb"))
						{
							std::fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, fp);
							std::fclose(fp);
							Console.WriteLn("GameRollback: link: EE RAM at attach -> %s", out.c_str());
						}
					}
					DoStart(static_cast<int>(RollbackDevice::Mode::Netplay), 8);
					s_net = true;
					s_net_fwd_save = -1;
					s_net_base_set = false;
					s_net_frame_base = 0;
					s_battle_counter++;
					s_detach_req = false;
					s_lphase = LinkPhase::Battle;
					Console.WriteLn("GameRollback: link: battle %u attached (%llu frames held for loads)", s_battle_counter,
						static_cast<unsigned long long>(s_held_frames));
					s_held_frames = 0;
					s_hold_frame = false;
					return false; // this frame is the session's frame 0
				}
				case LinkPhase::Battle:
					if (s_detach_req || NetBridge::RemoteDetachRequested())
					{
						// the session's stats as the battle ends (the status line reads "net: off" once detached)
						Console.WriteLn("GameRollback: link: battle %u end: %s", s_battle_counter, NetBridge::Status().c_str());
						s_lphase = LinkPhase::Detaching;
					}
					return false;
				case LinkPhase::Detaching:
				{
					const int d = NetBridge::Detach();
					if (d == 1 || d == 2 || d < 0)
					{
						DoStop();
						s_net = false;
						s_detach_req = false;
						s_onemore = true; // match end: the one-more menu follows
						s_choice_sent = false;
						s_vote_final = -1;
						s_vote_prev = 0;
						s_remote_in.clear();
						s_lphase = LinkPhase::Menu;
						return true;
					}
					return false; // keep running the session's frames until the agreed stop frame
				}
				default:
					return true;
			}
		}
		// Menu phases: the local player's port streams out, the remote player's port is fed from the stream (holding
		// the last input on underrun keeps autorepeat identical). The one-more menu reads port 0 only: there every peer
		// feeds its LOCAL player into port 0, so both drive their own cursor.
		// Seq_TickThreads hook: latch the one-more menu's script VM by its signature string
		void OneMoreOnTick(u32 vm)
		{
			const auto& L = s_man.link;
			if (!L.vote_cursor_var || L.vote_sig.empty())
				return;
			const u32 t0 = Rd(vm + 12), base = t0 ? Rd(t0 + 92) : 0;
			if (base && std::memcmp(&eeMem->Main[(base + L.vote_sig_off) & RAM_MASK], L.vote_sig.data(), L.vote_sig.size()) == 0)
				s_vote_vm = vm;
		}
		// port 0 of the one-more menu: our ○ becomes a vote (withheld, sent); with both votes the host drives the cursor
		void OneMoreVotePad(u8* buf)
		{
			const auto& L = s_man.link;
			s64 d = 0;
			if (!L.vote_cursor_var || !s_vote_vm || (Eval(L.onemore_decided, 0, &d) && d != 0))
				return;
			constexpr u16 OK = 0x20, UP = 0x1000, DOWN = 0x4000, LEFT = 0x8000, RIGHT = 0x2000;
			u8 raw[6];
			ReportToInput(buf, raw);
			u16 b = static_cast<u16>((raw[0] << 8) | raw[1]);
			const u16 pressed = static_cast<u16>(b & ~s_vote_prev);
			s_vote_prev = b;
			const u32 vars = Rd(Rd(s_vote_vm - 8 + 0x28));
			if (!vars)
				return;
			const u32 cursor = Rd(vars + 4 * L.vote_cursor_var);
			if (s_vote_final >= 0)
			{
				// both voted: move to the agreed option, then confirm (one press per two frames)
				const u32 target = s_vote_final == static_cast<s32>(L.retry_value) ? L.vote_retry_index : L.vote_css_index;
				b = (s_menu_frame & 1) ? 0 : (cursor != target ? DOWN : OK);
			}
			else if (!s_choice_sent)
			{
				if (pressed & OK)
				{
					s_choice_mine = cursor == L.vote_retry_index ? static_cast<s32>(L.retry_value) : (L.retry_value ? 0 : 1);
					NetBridge::LinkSend(MSG_CHOICE, s_menu_frame, &s_choice_mine, 4);
					s_choice_sent = true;
					Console.WriteLn("GameRollback: link: one-more vote %s sent", cursor == L.vote_retry_index ? "RETRY" : "CHARACTER SELECT");
				}
				b &= static_cast<u16>(~OK);
			}
			else
				b &= static_cast<u16>(~(OK | UP | DOWN | LEFT | RIGHT)); // voted: the menu stays live, the vote stays put
			const u8 x[6] = {static_cast<u8>(b >> 8), static_cast<u8>(b), 0x80, 0x80, 0x80, 0x80};
			BuildReport(x, buf);
		}
		bool LinkPadRead(u32 player, u8* buf)
		{
			if (player > 1)
				return false;
			if (s_pre_attach_neutral)
			{
				static constexpr u8 NEUTRAL[6] = {0, 0, 0x80, 0x80, 0x80, 0x80};
				BuildReport(NEUTRAL, buf);
				return true;
			}
			if (CssMirror::Enabled() && !s_onemore)
			{
				// async mirror menus: our own port is ours (0 latency), the other port is driven from the peer's
				// character-select record; nothing else is streamed
				u16 b = 0;
				if (static_cast<int>(player) == s_local)
				{
					if (PcInput::Active())
						b = PcInput::Buttons(s_local);
					else
					{
						u8 raw[6];
						ReportToInput(buf, raw);
						b = static_cast<u16>((raw[0] << 8) | raw[1]);
					}
				}
				b = CssMirror::Pad(player, b);
				const u8 x[6] = {static_cast<u8>(b >> 8), static_cast<u8>(b), 0x80, 0x80, 0x80, 0x80};
				BuildReport(x, buf);
				return true;
			}
			const u32 local_port = s_onemore ? 0u : static_cast<u32>(s_local);
			u8 in[6];
			if (s_onemore)
			{
				if (player != 0)
				{
					// the local player's own port is still read: keep its latest report for port 0
					if (static_cast<int>(player) == s_local)
					{
						std::memcpy(s_live_report[player], buf, PAD_REPORT);
						s_live_valid[player] = true;
					}
					return false;
				}
				if (PcInput::Active())
				{
					const u16 b = PcInput::Buttons(s_local);
					const u8 x[6] = {static_cast<u8>(b >> 8), static_cast<u8>(b), 0x80, 0x80, 0x80, 0x80};
					BuildReport(x, buf);
				}
				else if (s_local != 0 && s_live_valid[s_local])
					std::memcpy(buf, s_live_report[s_local], PAD_REPORT);
				OneMoreVotePad(buf);
				return true;
			}
			if (player == local_port)
			{
				if (PcInput::Active())
				{
					const u16 b = PcInput::Buttons(s_local);
					const u8 x[6] = {static_cast<u8>(b >> 8), static_cast<u8>(b), 0x80, 0x80, 0x80, 0x80};
					BuildReport(x, buf);
				}
				ReportToInput(buf, in);
				std::memcpy(s_live_report[player], buf, PAD_REPORT);
				s_live_valid[player] = true;
				NetBridge::LinkSend(MSG_INPUT, s_menu_frame, in, 6);
				return true;
			}
			if (!s_remote_in.empty())
			{
				std::memcpy(s_remote_last.data(), s_remote_in.front().data(), 6);
				s_remote_in.pop_front();
			}
			BuildReport(s_remote_last.data(), buf);
			return true;
		}

		void ApplyRequests()
		{
			// hot reload: poll the manifest's modification time about twice a second
			if (s_watch && s_attached && ++s_watch_tick >= 30)
			{
				s_watch_tick = 0;
				const s64 mt = MTime(s_path);
				if (mt && mt != s_mtime && s_net)
				{
					// a reload restarts the rollback state on this peer only: never during a netplay session
					Console.Warning("GameRollback: manifest changed during netplay: reload deferred to the session end");
				}
				else if (mt && mt != s_mtime)
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
			if (r.locks_set)
			{
				RemoveSessionLocks();
				if (r.locks_on)
					InstallSessionLocks();
			}
			if (r.net_stop && (s_net || s_lphase != LinkPhase::Off))
			{
				NetBridge::Stop();
				s_net = false;
				s_lphase = LinkPhase::Off;
				RemoveLinkHooks();
				RemoveSessionLocks();
				DoStop();
			}
			if (r.net_start && s_attached)
			{
				NetBridge::Host host;
				host.poll_local_input = [](int player, u8* out) {
					static constexpr u8 NEUTRAL[NetBridge::INPUT_SIZE] = {0, 0, 0x80, 0x80, 0x80, 0x80};
					if (PcInput::Active() && player >= 0 && player < 2)
					{
						const u16 b = PcInput::Buttons(player);
						const u8 in[NetBridge::INPUT_SIZE] = {static_cast<u8>(b >> 8), static_cast<u8>(b), 0x80, 0x80, 0x80, 0x80};
						std::memcpy(out, in, sizeof(in));
					}
					else if (player >= 0 && player < static_cast<int>(PAD_PLAYERS) && s_live_valid[player])
						ReportToInput(s_live_report[player], out);
					else
						std::memcpy(out, NEUTRAL, sizeof(NEUTRAL));
				};
				host.state_checksum = [] { return HashState(); };
				// desync evidence: the rollback ring (every frame it still holds) + live memory, once per battle, next to
				// the session recording -- a soak stops at the first desync and keeps this
				s_desync_prefix = r.net.replay_path;
				if (const size_t k = s_desync_prefix.find(";dump="); k != std::string::npos)
					s_desync_prefix.resize(k);
				if (s_desync_prefix.size() > 6 && s_desync_prefix.ends_with(".pcrep"))
					s_desync_prefix.resize(s_desync_prefix.size() - 6);
				s_desync_dumped_battle = ~0u;
				host.on_desync = [](int frame, u32 local, u32 remote, int kind) {
					if (s_desync_prefix.empty() || s_desync_dumped_battle == s_battle_counter)
						return;
					s_desync_dumped_battle = s_battle_counter;
					const std::string pre = fmt::format("{}.desync.b{}.frame{}", s_desync_prefix, s_battle_counter, frame);
					RollbackDevice::DumpRing(pre);
					Console.Error("GameRollback: DESYNC evidence (battle %u frame %d kind %d, local %08X remote %08X) -> %s.*.ee",
						s_battle_counter, frame, kind, local, remote, pre.c_str());
				};
				host.in_game = [] {
					s64 v = 1;
					return !s_man.gate_when || (Eval(*s_man.gate_when, 0, &v) && v != 0);
				};
				std::string err;
				s_dump_frames.clear();
				for (std::string* pth : {&r.net.journal_path, &r.net.replay_path})
					if (const size_t k = pth->find(";dump="); k != std::string::npos)
					{
						for (const std::string_view v : StringUtil::SplitString(std::string_view(*pth).substr(k + 6), ','))
							s_dump_frames.push_back(std::atoi(std::string(v).c_str()));
						pth->resize(k);
						s_dump_prefix = *pth;
					}
				if (!NetBridge::Start(r.net, host, &err))
					Console.Error("GameRollback: netplay start failed: %s", err.c_str());
				else if (r.net.mode == NetBridge::Mode::Link)
				{
					// async menus: no netcode until the first battle attach
					s_local = r.net.local_player;
					s_lphase = LinkPhase::Menu;
					s_net = false;
					s_battle_counter = 0;
					s_onemore = s_attach_req = s_detach_req = false;
					s_wait_hold = false;
					s_vote_final = -1;
					s_attach_frames = 0;
					s_commit_sel.clear();
					s_remote_in.clear();
					s_remote_pick_valid = false;
					s_remote_choice = -1;
					CssMirror::Configure(s_man.css_mirror);
					CssMirror::Reset(s_local, 0xC55u);
					InstallSessionLocks();
					InstallLinkHooks();
					InstallRollbackHooks(); // the simulation hooks run in menus too (see DoStop)
					for (bool& v : s_live_valid)
						v = false;
					Console.WriteLn("GameRollback: link mode: menus local, rollback attached per battle");
				}
				else
				{
					DoStart(static_cast<int>(RollbackDevice::Mode::Netplay), 8); // rollback depth fixed at 8
					s_net = true;
					InstallSessionLocks();
					s_net_fwd_save = -1;
					s_net_base_set = false;
					s_net_frame_base = 0;
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
		// Attaching the manifest that is already attached (unchanged on disk) is a no-op: a harness or UI that
		// attaches on demand must not restart the rollback state under a running session (the launcher attaches
		// from PS2RB_MANIFEST; a later Lua attach re-applied it on one peer mid-match = desync).
		auto norm = [](std::string p) {
			for (char& c : p)
				c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return p;
		};
		if (s_attached && norm(manifest_path) == norm(s_path) && MTime(s_path) == s_mtime)
			return true;
		if (s_net)
		{
			if (error)
				*error = "a netplay session is running: stop it before attaching another manifest";
			return false;
		}
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
		const std::string& journal, std::string* error)
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
		s_req.net.journal_path = journal;
		s_req.net.game_id = s_man.serial;
		s_req_pending = true;
		return true;
	}
	// the manifest's session lock-down outside a netplay session (harness tests): applied at the next frame boundary
	void LinkDumpAtAttach(const std::string& prefix) { s_link_dump_prefix = prefix; }
	void SessionLocks(bool on)
	{
		std::lock_guard lk(s_req_mtx);
		s_req.locks_set = true;
		s_req.locks_on = on;
		s_req_pending = true;
	}
	void NetStop()
	{
		std::lock_guard lk(s_req_mtx);
		s_req.net_stop = true;
		s_req_pending = true;
	}
	// Launcher contract (PovertyCaster PS2 host): environment variables read once, at the first state load (the
	// launcher boots with -statefile) or else the first presented frame
	//   PS2RB_MANIFEST  manifest path (attached automatically)
	//   PS2RB_NET       synctest | p2p        (netplay starts at the next frame boundary)
	//   PS2RB_LOCAL     local player 0/1       PS2RB_REMOTE  ip:port   PS2RB_PORT  local UDP port   PS2RB_DELAY  frames
	//   PS2RB_REPLAY    .pcrep to record (or, with PS2RB_NET=replay, to play)
	//   PS2RB_JOURNAL   host schedule journal to record (or, with PS2RB_NET=journal, to replay offline)
	void PollAutoStart()
	{
		static bool done = false;
		if (done)
			return;
		done = true;
		const char* man = std::getenv("PS2RB_MANIFEST");
		if (!man || !*man)
			return;
		std::string err;
		if (!Attach(man, &err))
		{
			Console.Error("GameRollback: PS2RB_MANIFEST %s: %s", man, err.c_str());
			return;
		}
		const char* net = std::getenv("PS2RB_NET");
		{
			// a PovertyCaster session: creamybinder owns local input in every mode (menus, offline, netplay), with the
			// per-game binding profile named after the manifest (fuc.yaml -> creamybinder-fuc.ini)
			PcInput::Params ip;
			std::string stem(Path::GetFileTitle(man));
			for (char& c : stem)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			ip.profile = stem;
			if (const char* d = std::getenv("PS2RB_CB_DIR"); d && *d)
				ip.config_dir = d;
			if (net && (std::string(net) == "p2p" || std::string(net) == "link"))
				if (const char* lp = std::getenv("PS2RB_LOCAL"); lp && *lp)
					ip.online_seat = std::atoi(lp);
			PcInput::Start(ip);
		}
		if (!net || !*net)
			return;
		auto env = [](const char* k, const char* def) {
			const char* v = std::getenv(k);
			return std::string(v && *v ? v : def);
		};
		const std::string m = net;
		const int mode = m == "p2p" ? 2 : m == "local" ? 1 : m == "replay" ? 3 : m == "journal" ? 4 : m == "link" ? 5 : 0;
		if (!NetStart(mode, std::atoi(env("PS2RB_LOCAL", "0").c_str()), env("PS2RB_REMOTE", ""),
				static_cast<u16>(std::atoi(env("PS2RB_PORT", "7000").c_str())),
				static_cast<u8>(std::atoi(env("PS2RB_DELAY", "2").c_str())), env("PS2RB_REPLAY", ""),
				env("PS2RB_JOURNAL", ""), &err))
			Console.Error("GameRollback: PS2RB_NET %s: %s", net, err.c_str());
		else
			Console.WriteLn("GameRollback: netplay %s requested from the environment", net);
	}

	std::string NetStatus() { return NetBridge::Status() + fmt::format(" | refused rollbacks {}", s_net_refused); }
	void OnStateLoaded()
	{
		// a savestate never lands inside a re-simulation, but the driver must not carry one over either
		s_driving = s_passthrough = s_pad_pending = s_replay_pending = false;
		// launcher boot (-statefile): start at the first frame boundary after the load, the same frame a harness
		// load_setup starts at, so a session's start state is the savestate itself (replays anchor on it)
		PollAutoStart();
	}
	std::string Status() { return s_status + fmt::format(" | mode {}", s_mode); }
	void SetFileWatch(bool on) { s_watch = on; }
	std::string LinkBadge()
	{
		std::lock_guard lk(s_badge_mtx);
		return s_badge;
	}
	// ---- game task profiler ----
	namespace
	{
		struct TaskCost
		{
			u64 calls[2] = {0, 0}, cycles[2] = {0, 0}, host_ns[2] = {0, 0}; // [0] normal, [1] re-simulated
		};
		struct TaskOpen
		{
			u64 key;
			u64 cycle;
			std::chrono::steady_clock::time_point t;
		};
		std::unordered_map<u64, TaskCost> s_tp_costs; // (script base << 32) | fn
		std::vector<u32> s_tp_vm_fns;
		std::vector<TaskOpen> s_tp_stack;
		u32 s_tp_call = 0, s_tp_ret = 0, s_tp_reg = 3;
		u64 s_tp_frames[2] = {0, 0};
	}
	bool TaskProfStart(u32 call_pc, u32 ret_pc, u32 fn_reg, std::vector<u32> vm_fns)
	{
		s_tp_vm_fns = std::move(vm_fns);
		TaskProfStop();
		s_tp_costs.clear();
		s_tp_stack.clear();
		s_tp_frames[0] = s_tp_frames[1] = 0;
		s_tp_call = call_pc;
		s_tp_ret = ret_pc;
		s_tp_reg = fn_reg & 31;
		EeHooks::AddCall(call_pc, [](u32) {
			const u32 fn = cpuRegs.GPR.r[s_tp_reg].UL[0];
			u64 key = fn;
			if (std::find(s_tp_vm_fns.begin(), s_tp_vm_fns.end(), fn) != s_tp_vm_fns.end())
				if (const u32 t0 = Rd(cpuRegs.GPR.n.a0.UL[0] + 8 + 12))
					key |= static_cast<u64>(Rd(t0 + 92)) << 32; // the VM's script base
			s_tp_stack.push_back({key, cpuRegs.cycle, std::chrono::steady_clock::now()});
			return EeHooks::Action::Continue;
		}, EeHooks::OWNER_GAME);
		EeHooks::AddCall(ret_pc, [](u32) {
			if (s_tp_stack.empty())
				return EeHooks::Action::Continue;
			const TaskOpen o = s_tp_stack.back();
			s_tp_stack.pop_back();
			const int r = RollbackDevice::IsResimulating() ? 1 : 0;
			TaskCost& c = s_tp_costs[o.key];
			c.calls[r]++;
			c.cycles[r] += cpuRegs.cycle - o.cycle;
			c.host_ns[r] += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - o.t).count());
			return EeHooks::Action::Continue;
		}, EeHooks::OWNER_GAME);
		Console.WriteLn("GameRollback: task profiler on (call %08X, return %08X, fn in r%u)", call_pc, ret_pc, s_tp_reg);
		return true;
	}
	void TaskProfStop()
	{
		if (!s_tp_call)
			return;
		EeHooks::Remove(s_tp_call);
		EeHooks::Remove(s_tp_ret);
		s_tp_call = s_tp_ret = 0;
		s_tp_stack.clear();
	}
	std::string TaskProfReport(u32 top_n)
	{
		std::vector<std::pair<u64, TaskCost>> v(s_tp_costs.begin(), s_tp_costs.end());
		u64 tot[2] = {0, 0};
		for (const auto& [fn, c] : v)
			for (int r = 0; r < 2; r++)
				tot[r] += c.host_ns[r];
		std::string out = "# game task profiler: inclusive per task function (nested lists count in their parent too)\n";
		for (int r = 1; r >= 0; r--)
		{
			std::sort(v.begin(), v.end(), [r](const auto& a, const auto& b) { return a.second.host_ns[r] > b.second.host_ns[r]; });
			out += fmt::format("\n## {} frames\n  host%    host_ms     calls   us/call    EEcyc/call  fn        script_base  script[0..32)\n",
				r ? "re-simulated" : "normal");
			u32 n = 0;
			for (const auto& [fn, c] : v)
			{
				if (!c.calls[r] || n++ >= top_n)
					continue;
				const u32 base = static_cast<u32>(fn >> 32);
				std::string head;
				for (u32 k = 0; base && k < 32; k++)
					head += fmt::format("{:02x}", eeMem->Main[(base + k) & RAM_MASK]);
				out += fmt::format("  {:5.1f}  {:9.2f}  {:8}  {:8.1f}  {:12}  {:08X}  {:08X}     {}\n", tot[r] ? 100.0 * c.host_ns[r] / tot[r] : 0.0,
					c.host_ns[r] / 1e6, c.calls[r], c.host_ns[r] / 1e3 / c.calls[r], c.cycles[r] / c.calls[r], static_cast<u32>(fn), base, head);
			}
		}
		return out;
	}
} // namespace GameRollback
