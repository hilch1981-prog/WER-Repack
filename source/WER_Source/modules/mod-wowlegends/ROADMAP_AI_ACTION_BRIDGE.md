# WOW LEGENDS — Engineering Roadmap: Talk-AND-Command + Semantic Layer

Owner: Kneuma. Stack: AzerothCore 3.3.5a + mod-playerbots + mod-wowlegends. Status: decision-ready, code-grounded. Date: 2026-07-05.

> ## ⚠️ VERIFICATION CORRECTIONS (2026-07-05, firsthand code read — these SUPERSEDE the assumptions below)
> **Build GO.** Crash-safety confirmed (orders are SEC_PLAYER, defer onto a queue, run in the bot's own tick — cannot re-enter #2474). Four fixes to honor:
> 1. **Prefix is `"$"`, NOT empty.** Dispatch `$follow`/`$attack`; read `GetOption("AiPlayerbot.CommandPrefix","$")` (playerbots.conf:2278/.dist:2280; mod-playerbots rejects unprefixed, PlayerbotAI.cpp:663-668). All "bare verb" language below is wrong.
> 2. **Do NOT use companion.cpp's `ParseCommands` for orders** — it only accepts `.`/`!` and silently no-ops a `$` string (Chat.cpp:250-256). Route via the **chat-hook `HandleCommand` path**: PlayerbotAI::HandleCommand (Playerbots.cpp:198) → `deferredChatCommands` (PlayerbotAI.cpp:977-983) → runs in the bot tick (:517, :571-620, driven Playerbots.cpp:169-175). Our aichat OnUpdate drain (aichat.cpp:1732) is a separate top-level frame.
> 3. **Target binding CONFIRMED**: set the owner's selection in-tick via pure-core `Player::SetSelection(guid)` (Player.cpp:11509, no mod-playerbots override); attack/tank/pull read the requester's UNIT_FIELD_TARGET. attack/tank bind to GetMaster() → Phase 1 OWNER-driven only.
> 4. **Verb corrections:** drop `come`/`guard` (don't exist); `focus heal` uses `+Name`/`-Name` param not selection; `pull rti` = raid icon; use `reset botAI` not bare `reset`; `summon` is player-usable. Registry source = ChatCommandHandlerStrategy.cpp:24-176.
>
> **First slice:** `$follow`, hardcoded (owner,bot) pair, dispatched via the chat-hook route from the world-thread drain, NO LLM — prove the pipe (bot follows, no crash/stall). Then `$attack` (proves SetSelection), then fan out Tier A.

## 1. The bet

"Bots that talk via LLM" is now commodity (mod-llm-chatter, mod-ollama-chat ship it free). The **#1 unowned gap** in the whole niche — named verbatim in every press writeup of the viral bot server — is that **you can't ask a bot in plain English to pull a specific enemy, wait for mana, or trade an item**. mod-ollama-chat literally blacklists command-shaped messages. We ship the leapfrog: **talk AND command** — natural-language → real bot ACTION, dispatched crash-safely through the in-tick order pipe we already own. Second pillar: an **"Ask the World" RAG assistant** (the one thing Old Man Warcraft has that we lack), grounded in our own docs, running entirely in the portal. Both compound our durable edges rivals can't quickly copy: consequence/relationship memory (grudges, warmth) and zero-setup hosted AI.

## 2. Feature 1 — Natural-language bot commands ("talk AND command")

### Architecture (end to end)

Reuse the existing AI-chat pipe verbatim — one new kind, one sibling parse, one dispatch block. **No new HTTP path, no second queue.**

```
WORLD THREAD (whisper hook, OnPlayerCanUseChat @ aichat.cpp:1470)
  IsBotOrder(msg)?  -> yes: existing explicit-order path, untouched
                    -> no + AiCommand.Enabled + grouped/owns-bot:
                       build compact TargetTable via Cell::VisitObjects
                       (slot,name,kind,hp%,dist,is_caster,is_elite,guid),
                       enqueue AiRequest kind=AI_KIND_ORDER=4  (all game reads happen HERE)
        |
        v
WORKER THREAD (pool 1-4, WorkerLoop @ aichat.cpp:1089)  — PURE network+string, zero game state
  ProviderGenerate(order shape) -> parse to AiResult{order, target_slot, confidence, ack}
  push onto g_out under g_outMtx  (ObjectGuid + strings only, never Player*/Unit*)
        |
        v
WORLD THREAD (OnUpdate drain @ aichat.cpp:1732)  — THE ONLY ACTUATOR
  re-resolve owner+bot via ObjectAccessor::FindConnectedPlayer(guid)
  re-resolve target_slot against a FRESH grid snapshot (world moved during ~1s round-trip)
  set player selection to the live Unit via a PURE CORE unit-field write
  ChatHandler(owner->GetSession()).ParseCommands("<bareVerb>")   // SEC_PLAYER, in-tick
  whisper the ack ("On it — pulling the caster")
```

Chassis is Design 1's minimal single-queue plumbing; grafted on are Design 2's world-thread TargetTable snapshot, confidence gate, and per-provider extractor tiers. The dispatch idiom is copied verbatim from `wowlegends_companion.cpp:246-250` (proven crash-safe).

### Order vocabulary — build ONLY from the verified mod-playerbots trigger registry

`CommandPrefix` defaults to **empty** (`PlayerbotAIConfig.cpp:420`). **Dispatch BARE verbs** (`attack`, `follow`), never `!attack`. **VERIFY `AiPlayerbot.CommandPrefix` on the deployed repack before finalizing the map** — a wrong prefix makes every order silently no-op.

**Ships clean now — no target, selection-independent:**

| Intent | Bare verb |
|---|---|
| follow / come with me | `follow` |
| stay / hold here | `stay` |
| flee / get out | `flee` |
| run away / disengage | `runaway` (alias `warning`) |
| kill anything / grind | `grind` |
| come here / regroup / on me | `summon` *(remapped — no `come` verb exists)* |
| res me / revive | `revive` |
| release spirit | `release` |
| go home / hearth | `home` |
| taxi / fly to | `taxi` |
| stop / cancel / stand down | `reset botAI` *(NOT bare `reset` — not a registered trigger)* |

**Selection-bound — dispatcher MUST set player selection to the resolved Unit FIRST, then dispatch the bare verb** (`attack`/`pull`/`focus heal` bind to `master->GetTarget()`, `AttackAction.cpp:32` — the order text cannot carry a target):

| Intent | Bare verb (after selection set) |
|---|---|
| pull / engage the caster / the elite | `attack` or `pull` (slot chosen by is_caster/is_elite/hp% descriptor) |
| tank this | `tank attack` |
| pull with raid marker | `pull rti` |
| focus heals on X | `focus heal` |
| assist me / attack my target | `attack` (player's current target — no resolve needed) |

**The one text-target verb:** `cast <spell> on <PlayerName>` — parses a **player** name via `FindPlayerByName` (`CastCustomSpellAction.cpp:41`). Players only; **never resolves mobs by descriptor**. Good for heal-me / shield-me on group members.

**Honest soft fallbacks (do NOT sell capability the engine lacks):**
- `wait for my mana` / `rest` / `drink` / `eat` → `stay` with an honest ack ("holding here"). There is no real rest/wait order — the bot just holds position.
- `trade` / `give me X` → **ack-only in v1**. `trade` exists but item pick is link/window-driven, not NL-addressable.

**GAP — no backing order, do NOT invent tokens:** `guard` / `defend` / `protect X` (guard is a movement *strategy*, not a chat trigger). Omit from the map.

### Provider strategy (three tiers, per-provider auto-detect)

`UseNativeTools=1` with fallback to strict-JSON. Universal parse target for every tier: strict-JSON system prompt + `StripThinkSafe` + first-`{`-to-matching-`}` brace extractor.

- **OpenAI (BYO key):** native tool-calling with `strict:true` Structured Outputs — schema *guaranteed*. `additionalProperties:false`, all params required, `parallel_tool_calls:false`. Primary/best.
- **Hosted DeepSeek proxy:** native tool-calling (OpenAI-compatible, confirmed) — requires **ONE proxy allowlist edit**: pass `tools`/`tool_choice` through to DeepSeek and `message.tool_calls` back. Until that ships, `UseNativeTools=0` → DeepSeek `json_object` mode. **Never combine `tools` + `json_object` in one request.** Still 1 credit/request.
- **Local Ollama:** do **NOT** rely on native tool-calling — llama3.2:3b-class models dump the call into `content`. Use the strict JSON-schema `format` parameter (constrains the token stream). For models too weak even for that, the optional embedding router (Feature 2) is the offline floor.

### Confidence gate + ack + failure guardrails

- **Confidence gate:** below `MinConfidence` → clarify ack listing supported verbs ("try: pull, tank, follow, stay, flee, cast"). **Never guess an action.**
- **Whitelist map = the injection boundary.** The LLM can only emit a token in the fixed map; any token not in the map is discarded. A crafted whisper cannot inject an arbitrary command string, affect other players, GM commands, or the DB — the sole actuator is a SEC_PLAYER order on the player's OWN bot.
- **Stale target:** re-resolve the slot against a fresh grid snapshot in OnUpdate; if the enemy died/moved, re-match by descriptor or abort with an ack ("that target's gone").
- **Ack always fires** even on a no-op, so the loop never feels dead.
- **Rate:** per-(owner,bot) cooldown + existing `g_botFloor` (2s).
- **Fail-safe degrade:** no parseable order key → treat as plain `AI_KIND_DIRECTED` chat.

### Crash-safety (#2474)

Immune **by construction**. The worker pool emits only `ObjectGuid`+strings on `g_out` — it never touches a `Player*`/`Unit*`, never calls a mod-playerbots Engine/strategy/`ChangeStrategy`/`Init` symbol. The sole bot actuator is `ChatHandler(owner->GetSession()).ParseCommands(verb)` invoked from `WORLDHOOK_ON_UPDATE` — a **top-level world-thread stack frame, NOT nested inside `Engine::DoNextAction`** — so it cannot re-enter the crash class (verified: worker 1089-1116, hook 1620-1625, drain 1732). The pre-order selection set **MUST be a pure core unit-field write** (the engine reads it next tick) — **NEVER** a mod-playerbots target/focus/strategy helper, which would re-enter the still-unguarded engine paths. Audit this actuator explicitly: confirm the SetTarget path calls no mod-playerbots symbol. **Standing code-review invariant:** every order verb — including future `cast`/`use`/`trade`/`wait for mana` — compiles to a `ParseCommands` text order from OnUpdate; no direct Engine/strategy/value/`ChangeStrategy`/`Init` call, ever.

### Config toggles (mod_wowlegends.conf.dist)

```
WowLegends.AiCommand.Enabled          = 0    # master, world-changing -> default OFF
WowLegends.AiCommand.MinConfidence    = 0.6  # below -> clarify ack, no action
WowLegends.AiCommand.UseNativeTools   = 1    # 0 = force strict-JSON (safety valve for un-patched proxy)
WowLegends.AiCommand.OrderCooldownSeconds = 4  # per owner+bot
WowLegends.AiCommand.MaxTargetsInPrompt   = 12  # cap TargetTable prompt tokens
WowLegends.AiCommand.AllowGroupBots   = 1    # 1 = any group bot; 0 = only your .companion
WowLegends.AiCommand.AckOnly.Trade    = 1    # trade acks but doesn't execute in v1
```
First release: **grouped/owned-bot-only** to keep blast radius tiny.

### Effort: **M+** (mapping + selection-resolution + provider fixtures). Ship this FIRST — it is the unowned NL→action leapfrog.

## 3. Feature 2 — Embeddings / semantic layer

**Hard correction across ALL embeddings work:** DeepSeek has **NO embeddings endpoint** (confirmed). Embed via **OpenAI `text-embedding-3-small`** ($0.02/1M tokens — cents for the whole corpus) or **local Ollama `nomic-embed-text`** (free), while chat generation stays on DeepSeek. Meter the combined query as **1 credit** — the embed cost folds in, no new billing primitive.

### DO #1 — "Ask the World" RAG assistant (strongest STRATEGIC play)

Closes the one named competitive gap. Runs **ENTIRELY in the portal/proxy** (PHP/Node we control) — **core stays dumb transport**.

- **Where it runs:** a reserved "Loremaster/Oracle" bot whose `ApiUrl` points at a proxy `/ask` endpoint (or a new AiKind that only changes the endpoint URL + whisper delivery). Ingestion + embedding + vector store + retrieve-then-generate all live on the proxy. The in-game side reuses the existing worker→g_out→OnUpdate pipe; the core never sees an embedding. Zero new crash surface.
- **What's embedded:** WL's own assets — `GM_COMMANDS.md`, `conf.dist` help blocks, changelog/roadmap, addon docs, Discord support KB, optional live world-DB via the wow-legends MCP. A few hundred chunks.
- **Vector store:** flat-file/JSON cosine on the proxy host — no DB at this scale. Escalate to sqlite-vec (fits the MariaDB shape) or pgvector only at millions of chunks.
- **Cost/credits:** corpus embed is a one-time/occasional batch (cents). Per query = 1 embed + 1 completion, metered as **1 credit** hosted, free on Ollama. Popular Q's cache at 0 LLM cost.
- **Payoff:** one corpus, three surfaces (in-game, website, Frostmind Discord). Turns Discord/EmuCoach support load into in-game self-serve — matches the "here's how YOU run it" posture.
- **Effort: M.** Ship THIRD.

### DO #2 — Semantic companion-memory recall (strongest DEFENSIVE play)

Today's recall is a recency FIFO — literally `ORDER BY id DESC LIMIT N` at `companion_memory.cpp:155-156`. Swap for **relevance retrieval**: add a `vec_blob` column, embed each rare/gated memory on write, cosine top-k over a few-hundred rows **on the world thread** at recall (pure read-side math — no engine call, no #2474 risk, Ollama-friendly). "Bots recall the RIGHT moment" vs "bots remember the newest." ~30-line retrieval swap; sharpens the one moat rivals can't quickly copy. **Effort: M (~30 lines).** Ship SECOND (right after the action bridge).

### DROP / DEFER

- **Offline embedding intent-router** (chat-vs-order pre-gate): DEMOTE to optional behind an independent `Embeddings.Enabled` toggle, OFF the critical path. Only earns its keep for tiny non-tool-calling local models; hosted/DeepSeek/OpenAI get better intent from native function-calling, and a **free keyword pre-gate** covers chat-vs-order without spending a credit. **The action bridge needs NO embeddings.**
- **Semantic ambient-line dedupe** for Living Chatter: pure polish. Ship LAST or never.

## 4. Phased plan

| Phase | Scope | Effort | Unlocks |
|---|---|---|---|
| **1 — Order-bridge MVP** | `AI_KIND_ORDER=4` + worker strict-JSON parse + OnUpdate dispatch. Clean verbs only (`follow`/`stay`/`flee`/`grind`/`summon`/`reset botAI`) + selection-bound `attack`/`focus heal` with world-thread TargetTable + selection set. strict-JSON on hosted DeepSeek (`json_object`) + Ollama (`format` schema). Confidence gate + honest ack. Grouped/owned-bot-only, default OFF. | **M** | The demo: whisper "pull the caster" → bot obeys. The leapfrog is live. |
| **2 — Native tools + full verb map + ack polish** | Native tool-calling on OpenAI (`strict:true`) + hosted DeepSeek (after the one proxy allowlist edit). Add `pull`/`pull rti`/`tank attack`, `cast <spell> on <player>`, honest soft fallbacks (`rest`→`stay`), trade ack-only. Per-(owner,bot) cooldown, `.aichat` orders counter. | **M** | Reliable extraction across providers; the full "talk AND command" story shippable to marketing. |
| **3 — Ask the World RAG** | Portal `/ask` endpoint: ingest WL docs → embed (OpenAI/Ollama) → flat-file cosine → retrieve-then-generate. Reserved Oracle bot + `.ask` addon command + website surface. Metered 1 credit. | **M** | Closes the Old Man Warcraft gap; in-game self-serve support; one corpus / three surfaces. |
| **4 — Semantic companion-memory recall** | `vec_blob` column, embed-on-write, cosine top-k recall on the world thread, behind `Embeddings.Enabled`. | **M** | "Bots recall the right moment" — the uncanny relationship-memory beat rivals can't copy. |

Optional/never: offline intent-router, ambient dedupe.

## 5. The marketing beat

Cut ONE clip once Phase 1-2 land, tying **memory + command + grudge** into a single arc:

> You walk up to a bot who fought beside you last week. It greets you by name and references the fight ("Still owe you for that Sunreaver pull"). You whisper, in plain English, *"pull just the caster on the left and wait for my mana."* It acks — *"On it, holding for your go"* — pulls exactly that mob, and holds. Cut to a battleground days later: the bot you once ganked **hunts you down for the kill**, warmth/grudge memory driving the target choice.

No other server in the niche can show a bot that **recognizes you → obeys a plain-English tactical order → remembers you well enough to come for revenge.** That's the 30-second clip for r/wowservers / Show HN / the launch trailer.

## 6. Risks & open questions

- **CommandPrefix (BLOCKER — verify first):** defaults to `''` in mod-playerbots. Confirm `AiPlayerbot.CommandPrefix` on the deployed repack before wiring; dispatch bare verbs unless WL explicitly set a prefix.
- **Selection-bound targets (REQUIRED, not optional):** `attack`/`pull`/`pull rti`/`tank attack`/`focus heal` bind to `master->GetTarget()`. The dispatcher must set selection to the resolved live Unit **before** the bare verb. This makes "SetTarget before order" mandatory.
- **The SetTarget actuator (audit explicitly):** the pre-order bind must be a **pure core unit-field write** — never a mod-playerbots target/focus/strategy helper (would re-enter the unguarded engine, #2474). Confirm the path calls no mod-playerbots symbol. Verify exact `attack` target-binding semantics against the *separate* mod-playerbots tree before wiring.
- **Four intents have no backing order:** `come/regroup`→remap `summon`; `guard/defend/protect`→**DROP** (movement strategy, no chat trigger); `rest/drink/wait for mana`→`stay` soft fallback with honest ack; `stop/cancel`→`reset botAI` (bare `reset` fails). Do not invent tokens.
- **Cast-on-mob is impossible:** `cast <spell> on <name>` resolves **players only** (`FindPlayerByName`). Do not promise NL target extraction for casting on a descriptor'd mob.
- **DeepSeek embeddings don't exist:** route all embedding through OpenAI `text-embedding-3-small` or Ollama `nomic-embed-text`; chat stays on DeepSeek.
- **Ollama native tool-calling unreliable:** small models emit the call as JSON in `content`. Use the strict JSON-schema `format` param; reserve native tools for OpenAI + hosted DeepSeek.
- **Hosted DeepSeek tool-calling needs the ONE proxy allowlist edit** (`tools`/`tool_choice` through, `message.tool_calls` back). Until then `UseNativeTools=0` → `json_object`. Never combine `tools` + `json_object`.
- **Standing invariant:** every current/future order verb compiles to a `ParseCommands` text order from OnUpdate — no direct Engine/strategy/value calls, ever. Enforce in code review.
- **Ship gates:** `WowLegends.AiCommand.Enabled` default OFF (world-changing), grouped/owned-bot-only on first release, whitelist-map injection boundary, cooldown + `g_botFloor(2s)`, honest acks.
- **Open — provider fixtures:** tool_calls response shape differs slightly across OpenAI vs DeepSeek vs the hosted proxy (`arguments` as JSON string vs object); need a real fixture from **each** provider (esp. the hosted proxy) before trusting the parser. strict-JSON fallback de-risks any misbehaving provider.
