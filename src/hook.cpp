#include "hook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "util.h"

#include "RE/E/ExtraCount.h"

#include "RE/B/BSTCreateFactoryManager.h"
#include "RE/I/IMessageBoxCallback.h"
#include "RE/I/InterfaceStrings.h"
#include "RE/M/MessageBoxData.h"

#ifdef GetObject
#	undef GetObject
#endif

namespace
{
	constexpr std::uint32_t kKey_M = RE::BSKeyboardDevice::Keys::kM;
	constexpr auto* kBottomBarAutosellText =
		"\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB2\xD1\x81\xD1\x91";
	constexpr auto* kGoldPickupSound = "ITMGoldUp";

	std::atomic_bool g_sellQueued{ false };
	std::atomic_int  g_pendingBatches{ 0 };
	std::atomic_bool g_confirmOpen{ false };
	std::optional<double> g_currentSellMult{};
	std::unordered_set<const RE::TESBoundObject*> g_zeroPriceObjects{};
	std::unordered_map<RE::FormID, std::int32_t>     g_menuUnitPriceByForm{};
	std::unordered_map<std::uint64_t, std::int32_t>  g_menuUnitPriceByFormValue{};

	[[nodiscard]] std::optional<RE::RefHandle> GetRefHandle(RE::TESObjectREFR* a_refr)
	{
		if (!a_refr) {
			return std::nullopt;
		}

		RE::RefHandle out = 0;
		RE::CreateRefHandle(out, a_refr);
		if (out == 0) {
			return std::nullopt;
		}

		return out;
	}

	[[nodiscard]] bool IsBarterMenuOpen()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		return ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
	}

	[[nodiscard]] RE::TESObjectREFR* GetMerchantContainer(RE::Actor* a_merchant)
	{
		if (!a_merchant) {
			return nullptr;
		}

		if (const auto vendorFaction = a_merchant->GetVendorFaction(); vendorFaction && vendorFaction->vendorData.merchantContainer) {
			return vendorFaction->vendorData.merchantContainer;
		}

		return a_merchant;
	}

	[[nodiscard]] bool IsExcludedFromAutosell(const RE::TESBoundObject* a_object)
	{
		if (!a_object) {
			return true;
		}

		const char* editorID = a_object->GetFormEditorID();
		return editorID && std::string_view{ editorID } == "Unarmed";
	}

	struct SellState
	{
		std::int32_t itemsSold{ 0 };
		std::int32_t goldGained{ 0 };
	};

	SellState g_state{};


	[[nodiscard]] std::int32_t ComputeUnitValue(std::int32_t a_entryValue, std::int32_t a_totalCount, std::int32_t a_baseValueHint)
	{
		a_entryValue = std::max<std::int32_t>(0, a_entryValue);

		(void)a_totalCount;
		(void)a_baseValueHint;
		return a_entryValue;
	}

	[[nodiscard]] std::optional<std::int32_t> ParseIntFromText(std::string_view a_text)
	{
		std::uint64_t value = 0;
		bool hasDigit = false;
		bool hasDashLike = false;

		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const unsigned char c = static_cast<unsigned char>(a_text[i]);
			if (c >= '0' && c <= '9') {
				hasDigit = true;
				value = value * 10u + static_cast<std::uint64_t>(c - '0');
				if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
					return std::numeric_limits<std::int32_t>::max();
				}
				continue;
			}

			if (c == '-') {
				hasDashLike = true;
			} else if (c == 0xE2 && i + 2 < a_text.size()) {
				const unsigned char c2 = static_cast<unsigned char>(a_text[i + 1]);
				const unsigned char c3 = static_cast<unsigned char>(a_text[i + 2]);
				if (c2 == 0x80 && (c3 == 0x93 || c3 == 0x94)) {
					hasDashLike = true;
				}
			}
		}

		if (!hasDigit) {
			return hasDashLike ? std::optional<std::int32_t>(0) : std::nullopt;
		}

		return static_cast<std::int32_t>(value);
	}

	[[nodiscard]] std::optional<std::int32_t> ParseIntFromGfxValueDepth(const RE::GFxValue& a_value, int a_depth)
	{
		if (a_value.IsNumber()) {
			return static_cast<std::int32_t>(std::llround(a_value.GetNumber()));
		}

		if (a_value.IsString()) {
			return ParseIntFromText(a_value.GetString());
		}

		if (a_depth <= 0) {
			return std::nullopt;
		}

		if (a_value.IsObject() || a_value.IsDisplayObject()) {
			constexpr std::array<const char*, 3> kTextKeys{ "text", "value", "label" };
			for (const auto* key : kTextKeys) {
				RE::GFxValue inner;
				if (a_value.GetMember(key, std::addressof(inner))) {
					if (const auto parsed = ParseIntFromGfxValueDepth(inner, a_depth - 1)) {
						return parsed;
					}
				}
			}

			std::optional<std::int32_t> found{};
			a_value.VisitMembers([&](const char*, const RE::GFxValue& v) {
				if (found) {
					return;
				}
				if (v.IsNumber() || v.IsString()) {
					found = ParseIntFromGfxValueDepth(v, a_depth - 1);
					return;
				}
				if ((v.IsObject() || v.IsDisplayObject()) && a_depth > 1) {
					found = ParseIntFromGfxValueDepth(v, a_depth - 1);
					return;
				}
			});
			if (found) {
				return found;
			}
		}

		return std::nullopt;
	}

	[[nodiscard]] std::optional<std::int32_t> ParseIntFromGfxValue(const RE::GFxValue& a_value)
	{
		return ParseIntFromGfxValueDepth(a_value, 2);
	}

	[[nodiscard]] std::optional<std::int32_t> TryGetGfxInt(const RE::GFxValue& a_obj, const char* a_name)
	{
		if (!a_obj.IsObject() && !a_obj.IsDisplayObject()) {
			return std::nullopt;
		}

		RE::GFxValue v;
		if (!a_obj.GetMember(a_name, std::addressof(v))) {
			return std::nullopt;
		}

		return ParseIntFromGfxValue(v);
	}

	[[nodiscard]] bool ContainsInsensitive(std::string_view a_haystack, std::string_view a_needle)
	{
		if (a_needle.empty() || a_haystack.size() < a_needle.size()) {
			return false;
		}

		auto toLower = [](unsigned char c) -> char {
			if (c >= 'A' && c <= 'Z') {
				return static_cast<char>(c - 'A' + 'a');
			}
			return static_cast<char>(c);
		};

		for (std::size_t i = 0; i + a_needle.size() <= a_haystack.size(); ++i) {
			bool match = true;
			for (std::size_t j = 0; j < a_needle.size(); ++j) {
				if (toLower(static_cast<unsigned char>(a_haystack[i + j])) != toLower(static_cast<unsigned char>(a_needle[j]))) {
					match = false;
					break;
				}
			}
			if (match) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] bool IsLikelyPriceField(std::string_view a_name)
	{
		return ContainsInsensitive(a_name, "price") ||
		       ContainsInsensitive(a_name, "value") ||
		       ContainsInsensitive(a_name, "barter") ||
		       ContainsInsensitive(a_name, "sell") ||
		       ContainsInsensitive(a_name, "cost") ||
		       ContainsInsensitive(a_name, "gold");
	}

	template <class Fn>
	void ScanGfxInts(const RE::GFxValue& a_value, int a_depth, int& a_budget, std::string_view a_key, Fn&& a_fn)
	{
		if (a_budget <= 0) {
			return;
		}
		--a_budget;

		if (a_value.IsNumber()) {
			a_fn(a_key, static_cast<std::int32_t>(std::llround(a_value.GetNumber())));
			return;
		}

		if (a_value.IsString()) {
			if (const auto parsed = ParseIntFromText(a_value.GetString())) {
				a_fn(a_key, *parsed);
			}
			return;
		}

		if (a_depth <= 0) {
			return;
		}

		if (a_value.IsArray()) {
			const auto size = static_cast<int>(a_value.GetArraySize());
			for (int i = 0; i < size; ++i) {
				RE::GFxValue elem;
				if (a_value.GetElement(static_cast<std::uint32_t>(i), std::addressof(elem))) {
					ScanGfxInts(elem, a_depth - 1, a_budget, a_key, a_fn);
				}
				if (a_budget <= 0) {
					return;
				}
			}
		}

		if (a_value.IsObject() || a_value.IsDisplayObject()) {
			a_value.VisitMembers([&](const char* a_name, const RE::GFxValue& a_child) {
				const std::string_view childKey = a_name ? std::string_view{ a_name } : std::string_view{};
				ScanGfxInts(a_child, a_depth - 1, a_budget, childKey, a_fn);
			});
		}
	}

	[[nodiscard]] std::optional<std::int32_t> GetDisplayedUnitPrice(const RE::GFxValue& a_obj, std::int32_t a_valueHint, std::int32_t a_count, bool a_allowZero, std::optional<double> a_ratioHint = std::nullopt)
	{
		constexpr std::array<const char*, 8> kCandidates{
			"value",
			"sellValue",
			"sellPrice",
			"price",
			"barterValue",
			"barterPrice",
			"displayValue",
			"displayPrice"
		};

		std::optional<std::int32_t> best{};
		double                     bestScore = std::numeric_limits<double>::infinity();
		bool                       sawZero = false;

		auto consider = [&](std::string_view keyName, std::int32_t price) {
			if (price == 0) {
				sawZero = true;
				return;
			}
			if (price < 0) {
				return;
			}

			if (a_valueHint > 0) {
				auto ratio = static_cast<double>(price) / static_cast<double>(a_valueHint);

				if (ratio > 1.25 && a_count > 1) {
					const auto normalized = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::llround(static_cast<double>(price) / static_cast<double>(a_count))));
					if (normalized > 0) {
						const auto r2 = static_cast<double>(normalized) / static_cast<double>(a_valueHint);
						if (r2 > 0.0 && r2 < 5.0 && r2 <= 1.25) {
							price = normalized;
							ratio = r2;
						}
					}
				}

				const bool plausible = ratio > 0.01 && ratio < 5.0;
				if (!plausible) {
					return;
				}

				double score = 0.0;
				if (a_ratioHint && *a_ratioHint > 0.0) {
					const auto target = *a_ratioHint;
					const auto diff = std::abs(ratio - target);
					score = diff;

					if (ratio < target * 0.25 || ratio > target * 4.0) {
						score += 10.0;
					}
				} else {
					score = (ratio <= 1.25) ? ratio : (ratio + 10.0);
					if (ratio < 0.03) {
						score += 2.0;
					}
				}

				if (!IsLikelyPriceField(keyName)) {
					score += 1.0;
				}

				if (score < bestScore) {
					bestScore = score;
					best = price;
				}
			} else {
				if (!best.has_value() || price < *best) {
					best = price;
				}
			}
		};

		for (const auto* key : kCandidates) {
			const auto v = TryGetGfxInt(a_obj, key);
			if (!v.has_value()) {
				continue;
			}

			consider(key, *v);
		}

		if (!best) {
			int budget = 800;
			ScanGfxInts(a_obj, 4, budget, std::string_view{}, [&](std::string_view key, std::int32_t v) {
				consider(key, v);
			});
		}

		if (!best && a_allowZero && sawZero) {
			return 0;
		}

		return best;
	}

	struct OfferData
	{
		std::unordered_set<const RE::TESBoundObject*> zeroPriceObjects{};
		std::unordered_map<RE::FormID, std::int32_t>     unitPriceByForm{};
		std::unordered_map<std::uint64_t, std::int32_t>  unitPriceByFormValue{};
		std::optional<double> sellMult{};
		std::size_t samples{ 0 };
		std::uint32_t totalMenuItems{ 0 };
	};

	[[nodiscard]] OfferData BuildOfferDataFromMenu(RE::BarterMenu& a_menu, RE::PlayerCharacter& a_player)
	{
		OfferData out{};

		const auto playerHandle = GetRefHandle(std::addressof(a_player));
		if (!playerHandle) {
			return out;
		}

		const auto* itemList = a_menu.GetRuntimeData().itemList;
		if (!itemList) {
			return out;
		}

		std::vector<double> samples;
		samples.reserve(32);
		out.zeroPriceObjects.reserve(128);
		out.unitPriceByForm.reserve(256);
		out.unitPriceByFormValue.reserve(256);
		out.totalMenuItems = itemList->items.size();

		std::size_t sampled = 0;

		for (auto* item : itemList->items) {
			if (!item) {
				continue;
			}

			if (item->data.owner != *playerHandle) {
				continue;
			}

			const auto* entry = item->data.objDesc;
			if (!entry) {
				continue;
			}

			const auto* object = entry->GetObject();
			if (!object) {
				continue;
			}

			if (object->IsGold() || object->IsKey() || object->IsLockpick()) {
				continue;
			}
			if (IsExcludedFromAutosell(object)) {
				continue;
			}

			const auto baseValue = std::max<std::int32_t>(0, object->GetGoldValue());

			const auto count = std::max<std::int32_t>(1, static_cast<std::int32_t>(item->data.GetCount()));
			const auto entryValue = std::max<std::int32_t>(0, entry->GetValue());
			const auto unitValue = ComputeUnitValue(entryValue, count, baseValue);

			if (const auto displayedAny = GetDisplayedUnitPrice(item->obj, std::max(baseValue, unitValue), count, true)) {
				const auto offerUnit = std::max<std::int32_t>(0, *displayedAny);

				const auto formID = object->GetFormID();
				out.unitPriceByForm.try_emplace(formID, offerUnit);
				const std::uint64_t fvKey = (static_cast<std::uint64_t>(formID) << 32) | static_cast<std::uint32_t>(unitValue);
				out.unitPriceByFormValue.insert_or_assign(fvKey, offerUnit);

				if (*displayedAny == 0) {
					out.zeroPriceObjects.insert(object);
				}
			}

			if (unitValue <= 0) {
				continue;
			}

			const auto displayed = GetDisplayedUnitPrice(item->obj, std::max(baseValue, unitValue), count, false);
			if (!displayed || *displayed <= 0) {
				continue;
			}

			const auto mult = static_cast<double>(*displayed) / static_cast<double>(unitValue);
			if (mult > 0.0 && mult < 5.0) {
				if (sampled < 40) {
					samples.push_back(mult);
					++sampled;
				}
			}
		}

		if (samples.empty()) {
			out.samples = 0;
			return out;
		}

		std::ranges::sort(samples);
		out.samples = samples.size();
		out.sellMult = samples[samples.size() / 2];

		for (auto* item : itemList->items) {
			if (!item) {
				continue;
			}
			if (item->data.owner != *playerHandle) {
				continue;
			}

			const auto* entry = item->data.objDesc;
			if (!entry) {
				continue;
			}
			const auto* object = entry->GetObject();
			if (!object) {
				continue;
			}
			if (object->IsGold() || object->IsKey() || object->IsLockpick()) {
				continue;
			}

			const auto count = std::max<std::int32_t>(1, static_cast<std::int32_t>(item->data.GetCount()));
			const auto entryValue = std::max<std::int32_t>(0, entry->GetValue());
			const auto unitValue = ComputeUnitValue(entryValue, count, object->GetGoldValue());

			const auto formID = object->GetFormID();
			const std::uint64_t fvKey = (static_cast<std::uint64_t>(formID) << 32) | static_cast<std::uint32_t>(unitValue);
			if (out.unitPriceByFormValue.contains(fvKey)) {
				continue;
			}

			const auto displayedAny = GetDisplayedUnitPrice(item->obj, std::max(object->GetGoldValue(), unitValue), count, true, out.sellMult);
			if (!displayedAny) {
				continue;
			}
			const auto offerUnit = std::max<std::int32_t>(0, *displayedAny);
			out.unitPriceByForm.try_emplace(formID, offerUnit);
			out.unitPriceByFormValue.insert_or_assign(fvKey, offerUnit);
			if (offerUnit == 0) {
				out.zeroPriceObjects.insert(object);
			}
		}

		return out;
	}

	void RefreshBarterMenuUI(RE::TESObjectREFR* a_player, RE::TESObjectREFR* a_merchant)
	{
		if (const auto ui = RE::UI::GetSingleton(); ui) {
			if (auto barterMenu = ui->GetMenu<RE::BarterMenu>(); barterMenu) {
				if (auto* itemList = barterMenu->GetRuntimeData().itemList; itemList) {
					if (a_player) {
						itemList->Update(a_player);
					}
					if (a_merchant) {
						itemList->Update(a_merchant);
					}
					itemList->Update();
				}
			}
		}

		if (auto* mq = RE::UIMessageQueue::GetSingleton(); mq) {
			mq->AddMessage(RE::BarterMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kInventoryUpdate, nullptr);
			mq->AddMessage(RE::BarterMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, nullptr);
		}
	}

	[[nodiscard]] bool IsStackFavorited(const RE::ExtraDataList* a_xList)
	{
		return a_xList && a_xList->HasType<RE::ExtraHotkey>();
	}

	[[nodiscard]] bool IsStackProtected(RE::ExtraDataList* a_xList)
	{
		if (!a_xList) {
			return false;
		}
		if (a_xList->GetWorn()) {
			return true;
		}
		if (a_xList->HasQuestObjectAlias()) {
			return true;
		}
		return false;
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

	[[nodiscard]] bool SellBatch(std::int32_t a_maxStacks)
	{
		if (!IsBarterMenuOpen()) {
			return false;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		RE::NiPointer<RE::Actor> merchant;
		if (!RE::Actor::LookupByHandle(RE::BarterMenu::GetTargetRefHandle(), merchant) || !merchant) {
			return false;
		}

		auto* merchantContainer = GetMerchantContainer(merchant.get());
		if (!merchantContainer) {
			return false;
		}

		auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);
		if (!gold) {
			return false;
		}

		const double sellMult = g_currentSellMult.value_or(0.1);

		std::int32_t stacksProcessed = 0;
		std::int32_t itemsSoldThisBatch = 0;

		const auto inventory = player->GetInventory();
		for (const auto& [object, data] : inventory) {
			if (stacksProcessed >= a_maxStacks) {
				break;
			}

			if (!object || !data.second) {
				continue;
			}

			if (object->IsGold() || object->IsKey() || object->IsLockpick()) {
				continue;
			}

			auto& entry = *data.second;
			const std::int32_t totalCount = std::max<std::int32_t>(0, data.first);
			if (totalCount <= 0) {
				continue;
			}

			const std::int32_t baseValue = std::max<std::int32_t>(0, object->GetGoldValue());
			const std::int32_t entryValue = std::max<std::int32_t>(0, entry.GetValue());

			const std::int32_t unitValue = ComputeUnitValue(entryValue, totalCount, baseValue);

			std::int32_t unitSellValue = 0;
			if (unitValue > 0) {
				const auto formID = object->GetFormID();
				const std::uint64_t fvKey = (static_cast<std::uint64_t>(formID) << 32) | static_cast<std::uint32_t>(unitValue);
				if (const auto itFV = g_menuUnitPriceByFormValue.find(fvKey); itFV != g_menuUnitPriceByFormValue.end()) {
					unitSellValue = std::max<std::int32_t>(0, itFV->second);
				} else if (const auto it2 = g_menuUnitPriceByForm.find(formID); it2 != g_menuUnitPriceByForm.end()) {
					unitSellValue = std::max<std::int32_t>(0, it2->second);
				} else if (g_zeroPriceObjects.contains(object)) {
					unitSellValue = 0;
				} else {
					const auto computed = static_cast<std::int32_t>(std::floor(static_cast<double>(unitValue) * sellMult + 1e-6));
					unitSellValue = std::max<std::int32_t>(0, computed);
				}
			}

			if (entry.extraLists) {
				std::int32_t accountedCount = 0;

				for (auto* xList : *entry.extraLists) {
					if (stacksProcessed >= a_maxStacks) {
						break;
					}
					if (!xList) {
						continue;
					}

					const auto stackCount = GetStackCount(xList);
					accountedCount += stackCount;
					if (stackCount <= 0) {
						continue;
					}

					if (IsStackFavorited(xList) || IsStackProtected(xList)) {
						continue;
					}
					const auto sellCount = stackCount;
					if (sellCount <= 0) {
						continue;
					}

					player->RemoveItem(object, sellCount, RE::ITEM_REMOVE_REASON::kSelling, xList, merchantContainer);
					if (unitSellValue > 0) {
						player->AddObjectToContainer(gold, nullptr, sellCount * unitSellValue, nullptr);
					}

					g_state.itemsSold += sellCount;
					g_state.goldGained += sellCount * unitSellValue;
					itemsSoldThisBatch += sellCount;
					++stacksProcessed;
				}

				const auto remaining = std::max<std::int32_t>(0, totalCount - accountedCount);
				if (remaining > 0 && stacksProcessed < a_maxStacks) {
					player->RemoveItem(object, remaining, RE::ITEM_REMOVE_REASON::kSelling, nullptr, merchantContainer);
					if (unitSellValue > 0) {
						player->AddObjectToContainer(gold, nullptr, remaining * unitSellValue, nullptr);
					}

					g_state.itemsSold += remaining;
					g_state.goldGained += remaining * unitSellValue;
					itemsSoldThisBatch += remaining;
					++stacksProcessed;
				}
			} else {
				if (entry.IsFavorited() || entry.IsWorn() || entry.IsQuestObject()) {
					continue;
				}

				const auto sellCount = totalCount;
				if (sellCount <= 0) {
					continue;
				}

				player->RemoveItem(object, sellCount, RE::ITEM_REMOVE_REASON::kSelling, nullptr, merchantContainer);
				if (unitSellValue > 0) {
					player->AddObjectToContainer(gold, nullptr, sellCount * unitSellValue, nullptr);
				}

				g_state.itemsSold += sellCount;
				g_state.goldGained += sellCount * unitSellValue;
				itemsSoldThisBatch += sellCount;
				++stacksProcessed;
			}
		}

		if (itemsSoldThisBatch > 0) {
			RefreshBarterMenuUI(player, merchantContainer);
		}

		return stacksProcessed >= a_maxStacks;
	}

	void ProcessSellLoop()
	{
		if (!g_sellQueued.load()) {
			return;
		}

		constexpr std::int32_t kStacksPerBatch = 20;
		const bool             shouldContinue = SellBatch(kStacksPerBatch);

		if (shouldContinue && IsBarterMenuOpen()) {
			g_pendingBatches.fetch_add(1);
			SKSE::GetTaskInterface()->AddTask([]() {
				g_pendingBatches.fetch_sub(1);
				ProcessSellLoop();
			});
			return;
		}

		const auto sold = g_state.itemsSold;
		const auto gold = g_state.goldGained;
		g_state = {};

		if (sold > 0) {
			RE::PlaySound(kGoldPickupSound);
			RE::DebugNotification("\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD0\xB6\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xBE\xD1\x88\xD0\xBB\xD0\xB0 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD1\x88\xD0\xBD\xD0\xBE");
			SKSE::log::info("Sold {} item(s) for {} gold", sold, gold);
		} else {
			RE::DebugNotification("\xD0\x9D\xD0\xB5\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C");
			SKSE::log::info("Nothing to sell");
		}

		g_currentSellMult.reset();
		g_zeroPriceObjects.clear();
		g_menuUnitPriceByForm.clear();
		g_menuUnitPriceByFormValue.clear();
		g_sellQueued.store(false);
	}

	void BeginAutosellFromUIThread()
	{
		if (!IsBarterMenuOpen()) {
			return;
		}

		bool expected = false;
		if (!g_sellQueued.compare_exchange_strong(expected, true)) {
			return;
		}

		g_state = {};
		g_currentSellMult.reset();
		g_zeroPriceObjects.clear();
		g_menuUnitPriceByForm.clear();
		g_menuUnitPriceByFormValue.clear();

		if (const auto ui = RE::UI::GetSingleton(); ui) {
			if (auto barterMenu = ui->GetMenu<RE::BarterMenu>(); barterMenu) {
				if (auto* player = RE::PlayerCharacter::GetSingleton(); player) {
					const auto offerData = BuildOfferDataFromMenu(*barterMenu, *player);
					g_zeroPriceObjects = offerData.zeroPriceObjects;
					g_menuUnitPriceByForm = offerData.unitPriceByForm;
					g_menuUnitPriceByFormValue = offerData.unitPriceByFormValue;
					g_currentSellMult = offerData.sellMult;
				}
			}
		}

		if (!g_currentSellMult) {
			g_currentSellMult = 0.1;
		}

		if (auto* task = SKSE::GetTaskInterface(); task) {
			task->AddTask([]() {
				ProcessSellLoop();
			});
		} else {
			g_sellQueued.store(false);
		}
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
					BeginAutosellFromUIThread();
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
			if (!IsBarterMenuOpen()) {
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

			auto* data = creator->Create();
			if (!data) {
				g_confirmOpen.store(false);
				return;
			}

			data->bodyText =
				"\xD0\x92\xD1\x8B \xD1\x83\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD\xD1\x8B \xD1\x87\xD1\x82\xD0\xBE \xD1\x85\xD0\xBE\xD1\x82\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB2\xD1\x81\xD1\x91?";
			data->buttonText.clear();
			data->buttonText.push_back("\xD0\x94\xD0\xB0");
			data->buttonText.push_back("\xD0\x9D\xD0\xB5\xD1\x82");
			data->type = 0;
			data->cancelOptionIndex = 1;
			data->callback.reset(new SellConfirmCallback());
			data->menuDepth = 10;
			data->optionIndexOffset = 0;
			data->useHtml = false;
			data->verticalButtons = false;
			data->isCancellable = true;
			data->QueueMessage();
		});
	}

	void EnsureBottomBarAutosellHint(RE::BarterMenu* a_menu)
	{
		if (!a_menu) {
			return;
		}

		auto& runtime = a_menu->GetRuntimeData();
		if (!runtime.itemList) {
			return;
		}

		auto* view = runtime.itemList->view.get();
		if (!view) {
			return;
		}

		RE::GFxValue buttonPanel;
		if (runtime.root.GetMember("navPanel", std::addressof(buttonPanel))) {
		} else {
			RE::GFxValue bottomBar;
			if (!runtime.root.GetMember("bottomBar", std::addressof(bottomBar))) {
				if (!runtime.root.GetMember("bottomBar_mc", std::addressof(bottomBar)) &&
					!runtime.root.GetMember("BottomBar_mc", std::addressof(bottomBar))) {
					return;
				}
			}
			if (!bottomBar.IsObject() && !bottomBar.IsDisplayObject()) {
				return;
			}
			if (!bottomBar.GetMember("buttonPanel", std::addressof(buttonPanel))) {
				return;
			}
		}
		if (!buttonPanel.IsObject() && !buttonPanel.IsDisplayObject()) {
			return;
		}

		RE::GFxValue buttons;
		if (!buttonPanel.GetMember("buttons", std::addressof(buttons)) || !buttons.IsArray()) {
			return;
		}

		auto getLabelText = [](RE::GFxValue& a_btn) -> std::string_view {
			RE::GFxValue label;
			if (a_btn.GetMember("label", std::addressof(label)) && label.IsString()) {
				const char* s = label.GetString();
				return s ? std::string_view{ s } : std::string_view{};
			}
			if (a_btn.GetMember("_label", std::addressof(label)) && label.IsString()) {
				const char* s = label.GetString();
				return s ? std::string_view{ s } : std::string_view{};
			}
			return {};
		};

		std::int32_t activeButtons = 0;
		bool hasAutosellHint = false;

		const auto count = static_cast<std::uint32_t>(buttons.GetArraySize());
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::GFxValue btn;
			if (!buttons.GetElement(i, std::addressof(btn))) {
				continue;
			}
			if (!btn.IsObject() && !btn.IsDisplayObject()) {
				continue;
			}

			const auto labelText = getLabelText(btn);
			if (!labelText.empty()) {
				++activeButtons;
				if (labelText == kBottomBarAutosellText) {
					hasAutosellHint = true;
				}
			} else {
				RE::GFxValue visible;
				if (btn.GetMember("_visible", std::addressof(visible)) && visible.IsBool() && visible.GetBool()) {
					++activeButtons;
				}
			}
		}

		if (hasAutosellHint) {
			return;
		}

		if (activeButtons >= static_cast<std::int32_t>(buttons.GetArraySize())) {
			return;
		}

		RE::GFxValue buttonData;
		view->CreateObject(std::addressof(buttonData));

		RE::GFxValue textValue;
		view->CreateString(std::addressof(textValue), kBottomBarAutosellText);
		buttonData.SetMember("text", textValue);

		RE::GFxValue controlsObj;
		view->CreateObject(std::addressof(controlsObj));
		controlsObj.SetMember("keyCode", RE::GFxValue(kKey_M));
		buttonData.SetMember("controls", controlsObj);

		RE::GFxValue args[1]{ buttonData };
		RE::GFxValue result;
		buttonPanel.Invoke("addButton", std::addressof(result), args, 1);

		RE::GFxValue updArgs[1]{ RE::GFxValue(true) };
		buttonPanel.Invoke("updateButtons", std::addressof(result), updArgs, 1);
	}

	struct BarterMenuHooks
	{
		static RE::UI_MESSAGE_RESULTS ProcessMessage_Thunk(RE::BarterMenu* a_this, RE::UIMessage& a_message)
		{
			const auto res = ProcessMessage_Original(a_this, a_message);
			EnsureBottomBarAutosellHint(a_this);
			return res;
		}

		static void AdvanceMovie_Thunk(RE::BarterMenu* a_this, float a_interval, std::uint32_t a_currentTime)
		{
			AdvanceMovie_Original(a_this, a_interval, a_currentTime);
			EnsureBottomBarAutosellHint(a_this);
		}

		static void PostDisplay_Thunk(RE::BarterMenu* a_this)
		{
			PostDisplay_Original(a_this);
			EnsureBottomBarAutosellHint(a_this);
		}

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BarterMenu[0] };
			ProcessMessage_Original = vtbl.write_vfunc(0x4, ProcessMessage_Thunk);
			AdvanceMovie_Original = vtbl.write_vfunc(0x5, AdvanceMovie_Thunk);
			PostDisplay_Original = vtbl.write_vfunc(0x6, PostDisplay_Thunk);
		}

		static inline REL::Relocation<decltype(ProcessMessage_Thunk)> ProcessMessage_Original;
		static inline REL::Relocation<decltype(AdvanceMovie_Thunk)>   AdvanceMovie_Original;
		static inline REL::Relocation<decltype(PostDisplay_Thunk)> PostDisplay_Original;
	};

	class AutosellMenuEventHandler final : public RE::MenuEventHandler
	{
	public:
		bool CanProcess(RE::InputEvent* a_event) override
		{
			if (!a_event) {
				return false;
			}
			if (!IsBarterMenuOpen()) {
				return false;
			}
			if (a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
				return false;
			}
			const auto* buttonEvent = a_event->AsButtonEvent();
			if (!buttonEvent) {
				return false;
			}
			return buttonEvent->GetDevice() == RE::INPUT_DEVICE::kKeyboard && buttonEvent->GetIDCode() == kKey_M;
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
