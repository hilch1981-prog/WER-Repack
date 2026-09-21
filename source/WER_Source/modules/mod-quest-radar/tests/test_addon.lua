-- Lua 5.1 offline regression tests. These mocks are not an in-game UI test.
local root = assert(arg[1], "addon directory required")
local locale = arg[2] or "koKR"
local checks, clock = 0, 100
local output, labels, requests = {}, {}, {}
local function check(condition, message)
    checks = checks + 1
    assert(condition, message)
end
function GetLocale() return locale end
function table.wipe(value) for key in pairs(value) do value[key] = nil end; return value end
function GetTime() return clock end
function UnitName() return "테스터" end
function GetCurrentMapContinent() return 1 end
function GetCurrentMapZone() return 2 end
function GetZoneText() return "시험 지역" end
function hooksecurefunc() end
function InterfaceOptions_AddCategory() end
function SendAddonMessage(...) requests[#requests + 1] = {...} end
DEFAULT_CHAT_FRAME = { AddMessage = function(_, text) output[#output + 1] = text end }
SlashCmdList = {}
UIParent = {}
Minimap = {}
local function noop() end
local methods = {}
function methods:SetScript(name, fn) self.scripts[name] = fn end
function methods:RegisterEvent() end
function methods:GetName() return self.frameName end
function methods:SetText(text) self.text = text; labels[#labels + 1] = text end
function methods:SetChecked(value) self.checked = value end
function methods:GetChecked() return self.checked end
function methods:SetValue(value) self.value = value end
function methods:GetValue() return self.value end
local function object(name)
    return setmetatable({frameName = name, scripts = {}}, {
        __index = function(_, key) return methods[key] or noop end,
    })
end
function methods:CreateFontString() return object(nil) end
function CreateFrame(_, name)
    local frame = object(name)
    frame.Text = object(nil)
    if name then
        _G[name] = frame
        for _, suffix in ipairs({"Text", "Low", "High"}) do _G[name .. suffix] = object(nil) end
    end
    return frame
end

for _, file in ipairs({"Locale.lua", "QuestRadar.lua", "Sync.lua", "Options.lua"}) do
    assert(loadfile(root .. "/" .. file))()
end
local QR = QuestRadar
local expected = locale == "koKR" and "퀘스트 레이더 사용" or "Activer QuestRadar"
check(QR.L["Activer QuestRadar"] == expected, "locale choice")
check(QR.L["untranslated-key"] == "untranslated-key", "missing translation fallback")
check(QuestRadarOptionsenabled.Text.text == expected, "native checkbox label")
local translations = 0
for original, translated in pairs(QR.L) do
    translations = translations + 1
    check(type(translated) == "string" and #translated > 0, "empty translation")
    local originalFormats, translatedFormats = {}, {}
    for token in string.gmatch(original, "%%[%d%.]*[a-zA-Z]") do originalFormats[#originalFormats + 1] = token end
    for token in string.gmatch(translated, "%%[%d%.]*[a-zA-Z]") do translatedFormats[#translatedFormats + 1] = token end
    check(table.concat(originalFormats) == table.concat(translatedFormats), "format placeholder changed")
end
check(translations == (locale == "koKR" and 46 or 0), "translation count")

local command = SlashCmdList.QUESTRADAR
command("off"); check(not QR.db.enabled, "off")
command("on"); check(QR.db.enabled, "on")
command("scale 1.4"); check(QR.db.scale == 1.4, "valid scale")
command("scale 5"); check(QR.db.scale == 1.4, "invalid scale preserved")
command("tracked"); check(not QR.db.onlyTracked, "tracked toggle")
command("completed"); check(not QR.db.showCompleted, "completed toggle")
command("edge"); check(not QR.db.showOffscreen, "edge toggle")
command("module"); check(not QR.db.useModule, "module off")
command("module"); check(QR.db.useModule, "module on")
command("status")
command("help")
local checkbox = QuestRadarOptionsenabled
checkbox.checked = false; checkbox.scripts.OnClick(checkbox)
check(not QR.db.enabled, "native UI updates same state")
checkbox.checked = true; checkbox.scripts.OnClick(checkbox)
check(QR.db.enabled, "native UI reenable")
local slider = QuestRadarOptionsScale
slider.settingValue = false
slider.scripts.OnValueChanged(slider, 1.2999999999)
check(QR.db.scale == 1.3, "slider rounding")
QuestRadarDriver.scripts.OnEvent(QuestRadarDriver, "PLAYER_LOGIN")

local sync = QuestRadarSyncFrame
local function send(message, sender, channel)
    sync.scripts.OnEvent(sync, "CHAT_MSG_ADDON", "QuestRadar", message,
        channel or "WHISPER", sender or "테스터")
end
local objective = "OBJ\t1\t100\t0\t10.0\t20.0\t3.0\t10.0\t20.0\t0.0\t0\t한국어 퀘스트"
send("ME\t1\t10.0\t20.0", "다른플레이어")
send(objective, "다른플레이어"); send("END\t1\t1", "다른플레이어")
check(QR.server == nil, "foreign sender rejected")
send("ME\t1\t10.0\t20.0", "테스터", "GUILD")
send(objective, "테스터", "GUILD"); send("END\t1\t1", "테스터", "GUILD")
check(QR.server == nil, "wrong channel rejected")
send("ME\t1\t10.0\t20.0"); send(objective); send("END\t1\t1")
check(QR.moduleSeen and #QR.server.objectives == 1, "valid server batch")
check(QR.server.objectives[1].title == "한국어 퀘스트", "Korean wire title preserved")
local valid = QR.server
send(objective); send("END\t1\t1")
check(QR.server == valid, "OBJ without ME rejected")
send("ME\t1\t10.0\t20.0"); send(objective); send("END\t1\t2")
check(QR.server == valid, "wrong count rejected")
send("ME\t1\t10.0\t20.0"); send(objective); send("END\t2\t1")
check(QR.server == valid, "wrong map rejected")
send("ME\t1\t10.0\t20.0"); send("OBJ\t1\t100\t0\tbad\t20\t3\t10\t20\t0\t0\tbad"); send("END\t1\t1")
check(QR.server == valid, "malformed objective rejected")
send("ME\t1\t1e999\t20"); send("END\t1\t0")
check(QR.server == valid, "infinite coordinate rejected")
send("ME\t1\t10\t20"); send(string.rep("X", 256)); send("END\t1\t0")
check(QR.server == valid, "oversize payload rejected")
check(QR.GetServerData() == valid, "fresh data available")
clock = clock + 16
check(QR.GetServerData() == nil, "stale data falls back")
QR.RequestSync(); local sent = #requests
QR.RequestSync(); check(#requests == sent, "request throttle")
clock = clock + 3; QR.RequestSync()
check(#requests == sent + 1, "request throttle expires")
send("ME\t1\t0\t0"); send("END\t1\t0")
check(QR.GetServerData() and #QR.server.objectives == 0, "zero coordinates and empty batch")
QR.db.useModule = false
check(QR.GetServerData() == nil, "module disabled fallback")
print("PASS: " .. locale .. " " .. checks .. " assertions; mocked addon load/UI/commands/protocol, not in-game")
