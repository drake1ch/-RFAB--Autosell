#include "log.h"
#include "hook.h"

namespace
{
	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Autosell::Install();
		}
	}
}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}
	return true;
}
