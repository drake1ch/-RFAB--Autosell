#include "hook.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "RE/B/BSTCreateFactoryManager.h"
#include "RE/E/ExtraCount.h"
#include "RE/E/ExtraHotkey.h"
#include "RE/I/IMessageBoxCallback.h"
#include "RE/I/InterfaceStrings.h"
#include "RE/M/MessageBoxData.h"
#ifdef GetObject
#	undef GetObject
#endif
namespace
{
	constexpr std::uint32_t kKeyM = RE::BSKeyboardDevice::Keys::kM;
	constexpr auto* kHintSellAll = "\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB2\xD1\x81\xD1\x91";
	constexpr auto* kMsgConfirm =
		"\xD0\x92\xD1\x8B \xD1\x83\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD\xD1\x8B \xD1\x87\xD1\x82\xD0\xBE \xD1\x85\xD0\xBE\xD1\x82\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB2\xD1\x81\xD1\x91?";
	constexpr auto* kMsgSold = "\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD0\xB6\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xBE\xD1\x88\xD0\xBB\xD0\xB0 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD1\x88\xD0\xBD\xD0\xBE";
	constexpr auto* kMsgNothing = "\xD0\x9D\xD0\xB5\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C";
	constexpr auto* kSoundGoldUp = "ITMGoldUp";
	std::atomic_bool g_sellQueued{ false };
	std::atomic_bool g_confirmOpen{ false };
	std::uintptr_t   g_hintMenuAddr{ 0 };
	bool             g_hintInjected{ false };
	struct SaleAction
	{
		RE::TESBoundObject* object{ nullptr };
		RE::ExtraDataList*  xList{ nullptr };
		std::int32_t        count{ 0 };
		std::int32_t        unitPrice{ 0 };
	};
	struct SellResult { std::int32_t sold{ 0 }; };
	[[nodiscard]] bool IsBarterOpen()
	{
		const auto ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
	}
	[[nodiscard]] RE::TESObjectREFR* GetMerchantContainer(RE::Actor* a_merchant)
	{
		if (!a_merchant) {
			return nullptr;
		}
		if (const auto faction = a_merchant->GetVendorFaction(); faction && faction->vendorData.merchantContainer) {
			return faction->vendorData.merchantContainer;
		}
		return a_merchant;
	}
	[[nodiscard]] bool IsExcludedObject(const RE::TESBoundObject* a_object)
	{
		if (!a_object || a_object->IsGold() || a_object->IsKey() || a_object->IsLockpick()) {
			return true;
		}
		const char* editorID = a_object->GetFormEditorID();
		if (!editorID) {
			return false;
		}
		const auto id = std::string_view{ editorID };
		return id == "Unarmed" || id == "RFAB_Skin_Leather_AdventurerBackPack" || id == "RFAB_Lampa";
	}
	[[nodiscard]] bool IsStackBlocked(RE::ExtraDataList* a_xList)
	{
		return a_xList && (a_xList->HasType<RE::ExtraHotkey>() || a_xList->GetWorn() || a_xList->HasQuestObjectAlias());
	}
	[[nodiscard]] std::int32_t GetStackCount(RE::ExtraDataList* a_xList)
	{
		if (!a_xList) {
			return 0;
		}
		if (const auto* extraCount = a_xList->GetByType<RE::ExtraCount>(); extraCount) {
			return std::max<std::int32_t>(1, extraCount->count);
		}
		return std::max<std::int32_t>(1, a_xList->GetCount());
	}
	[[nodiscard]] std::optional<std::int32_t> ParseInt(const RE::GFxValue& a_value, int a_depth = 2)
	{
		if (a_value.IsNumber()) {
			return static_cast<std::int32_t>(std::llround(a_value.GetNumber()));
		}
		if (a_value.IsString()) {
			const char* s = a_value.GetString();
			if (!s) {
				return std::nullopt;
			}
			std::uint64_t value = 0;
			bool          inNumber = false;
			for (const auto c : std::string_view{ s }) {
				if (c >= '0' && c <= '9') {
					inNumber = true;
					value = value * 10u + static_cast<std::uint64_t>(c - '0');
					if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
						return std::numeric_limits<std::int32_t>::max();
					}
					continue;
				}
				if (inNumber) {
					return static_cast<std::int32_t>(value);
				}
			}
			return inNumber ? std::optional<std::int32_t>(static_cast<std::int32_t>(value)) : std::nullopt;
		}
		if (a_depth <= 0 || (!a_value.IsObject() && !a_value.IsDisplayObject())) {
			return std::nullopt;
		}
		constexpr std::array<const char*, 4> kNestedKeys{ "value", "text", "label", "htmlText" };
		for (const auto* key : kNestedKeys) {
			RE::GFxValue nested;
			if (a_value.GetMember(key, std::addressof(nested))) {
				if (const auto parsed = ParseInt(nested, a_depth - 1)) {
					return parsed;
				}
			}
		}
		return std::nullopt;
	}
	[[nodiscard]] std::int32_t GetMenuUnitPrice(const RE::GFxValue& a_itemObj)
	{
		if (!a_itemObj.IsObject() && !a_itemObj.IsDisplayObject()) {
			return 0;
		}

		RE::GFxValue field;
		if (a_itemObj.GetMember("infoValue", std::addressof(field))) {
			if (const auto parsed = ParseInt(field)) {
				return std::max<std::int32_t>(0, *parsed);
			}
		}
		return 0;
	}
	[[nodiscard]] std::vector<SaleAction> BuildActions(RE::BarterMenu& a_menu)
	{
		std::vector<SaleAction> actions{};
		const auto* itemList = a_menu.GetRuntimeData().itemList;
		if (!itemList) {
			return actions;
		}
		const auto merchantHandle = RE::BarterMenu::GetTargetRefHandle();
		std::unordered_set<RE::InventoryEntryData*> seenEntries{};
		actions.reserve(itemList->items.size());
		for (auto* item : itemList->items) {
			if (!item || (merchantHandle != 0 && item->data.owner == merchantHandle)) {
				continue;
			}
			auto* entry = item->data.objDesc;
			if (!entry) {
				continue;
			}
			if (!seenEntries.insert(entry).second) {
				continue;
			}
			auto* object = entry->GetObject();
			if (IsExcludedObject(object)) {
				continue;
			}
			const auto unitPrice = GetMenuUnitPrice(item->obj);
			const auto totalCount = static_cast<std::int32_t>(item->data.GetCount());
			if (totalCount <= 0) {
				continue;
			}
			if (!entry->extraLists) {
				if (entry->IsFavorited() || entry->IsWorn() || entry->IsQuestObject()) {
					continue;
				}
				actions.push_back(SaleAction{ object, nullptr, totalCount, unitPrice });
				continue;
			}
			std::unordered_set<RE::ExtraDataList*> seenStacks{};
			std::int32_t accounted = 0;
			for (auto* xList : *entry->extraLists) {
				if (!xList) {
					continue;
				}
				if (!seenStacks.insert(xList).second) {
					continue;
				}
				const auto count = GetStackCount(xList);
				accounted += count;
				if (count <= 0 || IsStackBlocked(xList)) {
					continue;
				}
				actions.push_back(SaleAction{ object, xList, count, unitPrice });
			}
			const auto remaining = std::max<std::int32_t>(0, totalCount - accounted);
			if (remaining > 0) {
				actions.push_back(SaleAction{ object, nullptr, remaining, unitPrice });
			}
		}
		return actions;
	}
	void NotifySellResult(const SellResult& a_result)
	{
		if (a_result.sold > 0) {
			RE::PlaySound(kSoundGoldUp);
			RE::DebugNotification(kMsgSold);
		} else {
			RE::DebugNotification(kMsgNothing);
		}
	}
	void QueueBarterRefresh(const SellResult& a_result)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			NotifySellResult(a_result);
			g_sellQueued.store(false);
			return;
		}
		task->AddUITask([a_result]() {
			if (!IsBarterOpen()) {
				NotifySellResult(a_result);
				g_sellQueued.store(false);
				return;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				NotifySellResult(a_result);
				g_sellQueued.store(false);
				return;
			}
			auto menu = ui->GetMenu<RE::BarterMenu>();
			if (!menu) {
				NotifySellResult(a_result);
				g_sellQueued.store(false);
				return;
			}
			auto* itemList = menu->GetRuntimeData().itemList;
			if (!itemList || !itemList->view.get()) {
				NotifySellResult(a_result);
				g_sellQueued.store(false);
				return;
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton(); player) {
				itemList->Update(player);
			} else {
				itemList->Update();
			}
			NotifySellResult(a_result);
			g_sellQueued.store(false);
		});
	}
	void RunSellActions(const std::vector<SaleAction>& a_actions)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			g_sellQueued.store(false);
			return;
		}
		RE::NiPointer<RE::Actor> merchant;
		if (!RE::Actor::LookupByHandle(RE::BarterMenu::GetTargetRefHandle(), merchant) || !merchant) {
			g_sellQueued.store(false);
			return;
		}
		auto* merchantContainer = GetMerchantContainer(merchant.get());
		auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);
		if (!merchantContainer || !gold) {
			g_sellQueued.store(false);
			return;
		}
		SellResult result{};
		for (const auto& action : a_actions) {
			if (!action.object || action.count <= 0) {
				continue;
			}
			const auto available = std::max<std::int32_t>(0, player->GetItemCount(action.object));
			if (available <= 0) {
				continue;
			}
			auto requested = std::min(available, action.count);
			if (requested <= 0) {
				continue;
			}
			RE::ExtraDataList* removeList = nullptr;
			auto inventory = player->GetInventory([&action](RE::TESBoundObject& a_item) {
				return std::addressof(a_item) == action.object;
			});
			auto it = inventory.find(action.object);
			if (it == inventory.end()) {
				continue;
			}
			auto* entry = it->second.second.get();
			if (!entry) {
				continue;
			}
			if (action.xList) {
				if (!entry->extraLists) {
					continue;
				}
				for (auto* xList : *entry->extraLists) {
					if (!xList || xList != action.xList) {
						continue;
					}
					const auto count = GetStackCount(xList);
					if (count <= 0 || IsStackBlocked(xList)) {
						continue;
					}
					removeList = xList;
					requested = std::min(requested, count);
					break;
				}
				if (!removeList) {
					continue;
				}
			} else {
				if (entry->IsFavorited() || entry->IsWorn() || entry->IsQuestObject()) {
					continue;
				}
				std::int32_t genericCount = std::max<std::int32_t>(0, it->second.first);
				if (entry->extraLists) {
					for (auto* xList : *entry->extraLists) {
						if (!xList) {
							continue;
						}
						genericCount -= std::max<std::int32_t>(0, GetStackCount(xList));
					}
				}
				genericCount = std::max<std::int32_t>(0, genericCount);
				if (genericCount <= 0) {
					continue;
				}
				requested = std::min(requested, genericCount);
			}
			if (requested <= 0) {
				continue;
			}
			player->RemoveItem(action.object, requested, RE::ITEM_REMOVE_REASON::kSelling, removeList, merchantContainer);
			result.sold += requested;
			if (action.unitPrice > 0) {
				const auto gold64 = static_cast<std::int64_t>(requested) * static_cast<std::int64_t>(action.unitPrice);
				const auto goldDelta = static_cast<std::int32_t>(std::clamp<std::int64_t>(
					gold64, 0, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
				if (goldDelta > 0) {
					player->AddObjectToContainer(gold, nullptr, goldDelta, nullptr);
				}
			}
		}
		QueueBarterRefresh(result);
	}
	void StartAutosellFromUI()
	{
		if (!IsBarterOpen()) {
			return;
		}
		bool expected = false;
		if (!g_sellQueued.compare_exchange_strong(expected, true)) {
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			g_sellQueued.store(false);
			return;
		}
		auto menu = ui->GetMenu<RE::BarterMenu>();
		if (!menu) {
			g_sellQueued.store(false);
			return;
		}
		auto actions = BuildActions(*menu);
		if (actions.empty()) {
			RE::DebugNotification(kMsgNothing);
			g_sellQueued.store(false);
			return;
		}
		RunSellActions(actions);
	}
	class SellConfirmCallback final : public RE::IMessageBoxCallback
	{
	public:
		SellConfirmCallback()
		{
			unk0C = 0;
		}
		void Run(Message a_msg) override
		{
			g_confirmOpen.store(false);
			if (a_msg != Message::kUnk0) {
				return;
			}
			if (auto* task = SKSE::GetTaskInterface(); task) {
				task->AddUITask([]() {
					StartAutosellFromUI();
				});
			}
		}
	};
	void ShowSellConfirmation()
	{
		bool expected = false;
		if (!g_confirmOpen.compare_exchange_strong(expected, true)) {
			return;
		}
		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			g_confirmOpen.store(false);
			return;
		}
		task->AddUITask([]() {
			if (!IsBarterOpen()) {
				g_confirmOpen.store(false);
				return;
			}
			auto* strings = RE::InterfaceStrings::GetSingleton();
			auto* factory = RE::MessageDataFactoryManager::GetSingleton();
			if (!strings || !factory) {
				g_confirmOpen.store(false);
				return;
			}
			const auto* creator = factory->GetCreator<RE::MessageBoxData>(strings->messageBoxData);
			if (!creator) {
				g_confirmOpen.store(false);
				return;
			}
			auto* box = creator->Create();
			if (!box) {
				g_confirmOpen.store(false);
				return;
			}
			box->bodyText = kMsgConfirm;
			box->buttonText.clear();
			box->buttonText.push_back("\xD0\x94\xD0\xB0");
			box->buttonText.push_back("\xD0\x9D\xD0\xB5\xD1\x82");
			box->type = 0;
			box->cancelOptionIndex = 1;
			box->callback.reset(new SellConfirmCallback());
			box->menuDepth = 10;
			box->optionIndexOffset = 0;
			box->useHtml = false;
			box->verticalButtons = false;
			box->isCancellable = true;
			box->QueueMessage();
		});
	}
	[[nodiscard]] bool GetButtonPanel(RE::BarterMenu& a_menu, RE::GFxValue& a_panel)
	{
		auto& rt = a_menu.GetRuntimeData();
		if (!rt.itemList || !rt.itemList->view.get()) {
			return false;
		}
		if (!rt.root.IsObject() && !rt.root.IsDisplayObject()) {
			return false;
		}
		if (rt.root.GetMember("navPanel", std::addressof(a_panel))) {
			return a_panel.IsObject() || a_panel.IsDisplayObject();
		}
		RE::GFxValue bar;
		if (!rt.root.GetMember("bottomBar", std::addressof(bar)) &&
			!rt.root.GetMember("bottomBar_mc", std::addressof(bar)) &&
			!rt.root.GetMember("BottomBar_mc", std::addressof(bar))) {
			return false;
		}
		if ((!bar.IsObject() && !bar.IsDisplayObject()) || !bar.GetMember("buttonPanel", std::addressof(a_panel))) {
			return false;
		}
		return a_panel.IsObject() || a_panel.IsDisplayObject();
	}
	void EnsureAutosellHint(RE::BarterMenu* a_menu)
	{
		if (!a_menu) {
			g_hintInjected = false;
			g_hintMenuAddr = 0;
			return;
		}
		const auto menuAddr = reinterpret_cast<std::uintptr_t>(a_menu);
		if (g_hintInjected && g_hintMenuAddr == menuAddr) {
			return;
		}
		RE::GFxValue panel;
		if (!GetButtonPanel(*a_menu, panel)) {
			return;
		}
		RE::GFxValue buttons;
		if (!panel.GetMember("buttons", std::addressof(buttons)) || !buttons.IsArray()) {
			return;
		}
		for (std::uint32_t i = 0; i < buttons.GetArraySize(); ++i) {
			RE::GFxValue btn;
			RE::GFxValue label;
			if (!buttons.GetElement(i, std::addressof(btn)) || (!btn.IsObject() && !btn.IsDisplayObject())) {
				continue;
			}
			if ((btn.GetMember("label", std::addressof(label)) || btn.GetMember("_label", std::addressof(label))) && label.IsString()) {
				const char* text = label.GetString();
				if (text && std::string_view{ text } == kHintSellAll) {
					g_hintInjected = true;
					g_hintMenuAddr = menuAddr;
					return;
				}
			}
		}
		auto* view = a_menu->GetRuntimeData().itemList->view.get();
		if (!view) {
			return;
		}
		RE::GFxValue data;
		RE::GFxValue text;
		RE::GFxValue controls;
		view->CreateObject(std::addressof(data));
		view->CreateString(std::addressof(text), kHintSellAll);
		data.SetMember("text", text);
		view->CreateObject(std::addressof(controls));
		controls.SetMember("keyCode", RE::GFxValue(kKeyM));
		data.SetMember("controls", controls);
		RE::GFxValue result;
		RE::GFxValue args[1]{ data };
		panel.Invoke("addButton", std::addressof(result), args, 1);
		RE::GFxValue upd[1]{ RE::GFxValue(true) };
		panel.Invoke("updateButtons", std::addressof(result), upd, 1);
		g_hintInjected = true;
		g_hintMenuAddr = menuAddr;
	}
	struct BarterMenuHooks
	{
		static void AdvanceMovie_Thunk(RE::BarterMenu* a_this, float a_interval, std::uint32_t a_currentTime)
		{
			if (AdvanceMovie_Original.address() != 0) {
				AdvanceMovie_Original(a_this, a_interval, a_currentTime);
			}
			EnsureAutosellHint(a_this);
		}
		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BarterMenu[0] };
			AdvanceMovie_Original = vtbl.write_vfunc(0x5, AdvanceMovie_Thunk);
		}
		static inline REL::Relocation<decltype(AdvanceMovie_Thunk)> AdvanceMovie_Original;
	};
	class AutosellMenuEventHandler final : public RE::MenuEventHandler
	{
	public:
		bool CanProcess(RE::InputEvent* a_event) override
		{
			if (!a_event || !IsBarterOpen() || a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
				return false;
			}
			const auto* button = a_event->AsButtonEvent();
			return button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == kKeyM;
		}
		bool ProcessButton(RE::ButtonEvent* a_event) override
		{
			if (!a_event || !a_event->IsDown()) {
				return false;
			}
			if (g_sellQueued.load() || g_confirmOpen.load()) {
				return true;
			}
			ShowSellConfirmation();
			return true;
		}
	};
	AutosellMenuEventHandler g_menuHandler;
}
namespace Autosell
{
	void Install()
	{
		auto* menuControls = RE::MenuControls::GetSingleton();
		if (!menuControls) {
			SKSE::log::error("MenuControls not available; autosell disabled");
			return;
		}
		menuControls->RegisterHandler(&g_menuHandler);
		BarterMenuHooks::Install();
		SKSE::log::info("Autosell installed (key: M)");
	}
}
