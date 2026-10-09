# NVIDIA API setup — required for external LLM bot chat

[한국어](NVIDIA_API.md) · **English**

> [!WARNING]
> **🟨 Try WER's NVIDIA LLM conversations.** Use your own valid API key.
> A key is required for **external LLM chat**, not for basic server startup or ordinary bot AI.
> Hosted inference avoids a local large-model installation. Free prototype access is not unlimited capacity or a production-service guarantee.

## Obtain a key

Open [NVIDIA Build](https://build.nvidia.com/), choose an available chat model and use its **Generate/Get API Key** control.
Sign in with your own account and complete the displayed access/terms checks. Store the key privately.
For changing screens, use the [official quickstart](https://docs.api.nvidia.com/nim/docs/api-quickstart).
The currently documented model is [nvidia/nemotron-3-ultra-550b-a55b](https://build.nvidia.com/nvidia/nemotron-3-ultra-550b-a55b).
Its page currently offers a prototype endpoint; verify your account's actual availability, limits and permitted use.

No shared key is included or provided by WER. Never publish a key in GitHub, chat, a screenshot or an issue.
No verified evidence supports promising dedicated “24 RTX 5080 GPUs,” unlimited free requests or problem-free 2,000-bot LLM operation.

## Edit the real config keys

Open **`configs/modules/mod_wowlegends.conf`** in the executable repack.
Change the existing values; do not add duplicate keys:

```ini
WowLegends.AiChat.Enabled = 1
WowLegends.AiChat.Provider = "openai"
WowLegends.AiChat.ApiUrl = "https://integrate.api.nvidia.com/v1/chat/completions"
WowLegends.AiChat.ApiKey = "YOUR_NVIDIA_API_KEY"
WowLegends.AiChat.Model = "nvidia/nemotron-3-ultra-550b-a55b"
```

`Provider = "openai"` selects the **OpenAI-compatible protocol**, not the company billing the request.
The URL above calls NVIDIA and requires a NVIDIA key. Enter only the raw key, without a `Bearer ` prefix; the implementation adds its header.
The WER display name does **not** rename the internal `WowLegends.*` keys or the `mod_wowlegends.conf` file.
Use an exact provider API model ID if choosing a different supported model.

Save before first launch. For an already running server, schedule maintenance, shut down the world gracefully and restart its launcher;
editing a config file alone does not alter settings already held in memory.
This guide does not authorize an assistant to restart a live server automatically.

## Test gradually

Talk on ordinary `/1` or `/s`, whisper, party, raid or guild channels where eligible bots can hear you.
No special `.world` command or world-chat mode is required. Proximity, membership, cooldowns, queue capacity and provider latency affect responses.
Start with a small test; replies to every line and instant replies are not guaranteed.

In this implementation, `WowLegends.AiChat.MaxTokens = 0` omits the output token cap from requests.
It does not remove provider/model limits. The documented timeout is `TimeoutMs = 12000`; slow requests can fail.
Do not switch ModelFast to an unrelated provider's model ID without checking compatibility.
Standalone `mod-ollama-chat` remains excluded from the WER build.

## Privacy, limits and errors

Enabling external chat sends the selected conversation and character/situation context to the provider.
Inform participants, review applicable data/production-use terms, and do not send sensitive information in game chat.
Bot-to-bot and raid conversations can increase request volume. Revoke/rotate a leaked key through the provider.

| Response / symptom | Check |
|---|---|
| 401/403 | Key validity, copying mistakes and model access |
| 404/model error | URL and exact API model ID |
| 429 | Request rate, account/model limits and available usage |
| Timeout/connection | Connectivity, latency, provider availability, DNS/proxy/TLS |
| No replies | Enabled setting/key, world restart, eligibility and queues |
| English/inaccurate replies | Locale/prompt/model behavior; complete localization or tactical correctness is not guaranteed |

Review `logs/Errors.log`, removing keys and private dialogue/account/IP data before sharing.
Do not disable certificate verification to bypass TLS errors. WER did not issue keys or make paid test calls on your behalf.
