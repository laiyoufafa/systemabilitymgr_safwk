/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "system_ability.h"

#include <cinttypes>

#include "datetime_ex.h"
#include "errors.h"
#include "hitrace_meter.h"
#include "if_system_ability_manager.h"
#include "iservice_registry.h"
#include "local_ability_manager.h"
#include "safwk_log.h"
#include "string_ex.h"

namespace OHOS {
namespace {
const std::string TAG = "SystemAbility";
}

SystemAbility::SystemAbility(bool runOnCreate)
{
    isRunning_ = false;
    abilityState_ = SystemAbilityState::NOT_LOADED;
    isRunOnCreate_ = runOnCreate;
    isDistributed_ = false;
    dumpLevel_ = 0;
    // timeout for waiting dependency ready, which configed in xml, with DEFAULT_DEPENDENCY_TIMEOUT(6s) by default
    dependTimeout_ = DEFAULT_DEPENDENCY_TIMEOUT;
    capability_ = u"";
}

SystemAbility::SystemAbility(int32_t systemAbilityId, bool runOnCreate) : SystemAbility(runOnCreate)
{
    saId_ = systemAbilityId;
}

SystemAbility::~SystemAbility()
{
    HILOGI(TAG, "SA:%{public}d destroyed", saId_);
}

bool SystemAbility::MakeAndRegisterAbility(SystemAbility* systemAbility)
{
    HILOGD(TAG, "registering system ability...");
    return LocalAbilityManager::GetInstance().AddAbility(systemAbility);
}

bool SystemAbility::AddSystemAbilityListener(int32_t systemAbilityId)
{
    HILOGD(TAG, "SA:%{public}d, listenerSA:%{public}d", systemAbilityId, saId_);
    return LocalAbilityManager::GetInstance().AddSystemAbilityListener(systemAbilityId, saId_);
}

bool SystemAbility::RemoveSystemAbilityListener(int32_t systemAbilityId)
{
    HILOGD(TAG, "SA:%{public}d, listenerSA:%{public}d", systemAbilityId, saId_);
    return LocalAbilityManager::GetInstance().RemoveSystemAbilityListener(systemAbilityId, saId_);
}

bool SystemAbility::Publish(sptr<IRemoteObject> systemAbility)
{
    if (systemAbility == nullptr) {
        HILOGE(TAG, "systemAbility is nullptr");
        return false;
    }
    HILOGD(TAG, "[PerformanceTest] SAFWK Publish systemAbilityId:%{public}d", saId_);
    int64_t begin = GetTickCount();
    sptr<ISystemAbilityManager> samgrProxy = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgrProxy == nullptr) {
        HILOGE(TAG, "failed to get samgrProxy");
        return false;
    }

    publishObj_ = systemAbility;
    ISystemAbilityManager::SAExtraProp saExtra(GetDistributed(), GetDumpLevel(), capability_, permission_);
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    int32_t result = samgrProxy->AddSystemAbility(saId_, publishObj_, saExtra);
    HILOGI(TAG, "[PerformanceTest] SAFWK Publish SA:%{public}d result : %{public}d, spend:%{public}" PRId64 " ms",
        saId_, result, (GetTickCount() - begin));
    if (result == ERR_OK) {
        abilityState_ = SystemAbilityState::ACTIVE;
        return true;
    }
    return false;
}

bool SystemAbility::CancelIdle()
{
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    if (abilityState_ != SystemAbilityState::IDLE) {
        return true;
    }
    sptr<ISystemAbilityManager> samgrProxy = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgrProxy == nullptr) {
        HILOGE(TAG, "failed to get samgrProxy");
        return false;
    }
    int32_t result = samgrProxy->CancelUnloadSystemAbility(saId_);
    return result == ERR_OK;
}

void SystemAbility::StopAbility(int32_t systemAbilityId)
{
    HILOGD(TAG, "SA:%{public}d", systemAbilityId);

    sptr<ISystemAbilityManager> samgrProxy = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgrProxy == nullptr) {
        HILOGE(TAG, "failed to get samgrProxy");
        return;
    }

    int32_t ret = samgrProxy->RemoveSystemAbility(systemAbilityId);
    HILOGI(TAG, "%{public}s to remove ability", (ret == ERR_OK) ? "success" : "failed");
}

void SystemAbility::Start()
{
    HILOGD(TAG, "starting system ability...");
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    if (abilityState_ != SystemAbilityState::NOT_LOADED) {
        return;
    }
    HILOGD(TAG, "[PerformanceTest] SAFWK OnStart systemAbilityId:%{public}d", saId_);
    int64_t begin = GetTickCount();
    HITRACE_METER_NAME(HITRACE_TAG_SAMGR, ToString(saId_) + "_OnStart");
    std::unordered_map<std::string, std::string> startReason = LocalAbilityManager::GetInstance().GetStartReason(saId_);
    OnStart(startReason);
    isRunning_ = true;
    HILOGI(TAG, "[PerformanceTest] SAFWK OnStart systemAbilityId:%{public}d finished, spend:%{public}" PRId64 " ms",
        saId_, (GetTickCount() - begin));
}

void SystemAbility::Idle(const std::unordered_map<std::string, std::string>& idleReason,
    int32_t& delayTime)
{
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    if (abilityState_ != SystemAbilityState::ACTIVE) {
        HILOGE(TAG, "[PerformanceTest] SAFWK cannot idle systemAbilityId:%{public}d", saId_);
        return;
    }
    HILOGD(TAG, "[PerformanceTest] SAFWK Idle systemAbilityId:%{public}d", saId_);
    int64_t begin = GetTickCount();
    delayTime = OnIdle(idleReason);
    if (delayTime == 0) {
        abilityState_ = SystemAbilityState::IDLE;
    }
    HILOGI(TAG, "[PerformanceTest] SAFWK Idle systemAbilityId:%{public}d finished, spend:%{public}" PRId64 " ms",
        saId_, (GetTickCount() - begin));
}

void SystemAbility::Active(const std::unordered_map<std::string, std::string>& activeReason)
{
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    if (abilityState_ != SystemAbilityState::IDLE) {
        return;
    }
    HILOGD(TAG, "[PerformanceTest] SAFWK Active systemAbilityId:%{public}d", saId_);
    int64_t begin = GetTickCount();
    OnActive(activeReason);
    abilityState_ = SystemAbilityState::ACTIVE;
    HILOGI(TAG, "[PerformanceTest] SAFWK Active systemAbilityId:%{public}d finished, spend:%{public}" PRId64 " ms",
        saId_, (GetTickCount() - begin));
}

void SystemAbility::Stop()
{
    HILOGD(TAG, "stopping system ability...");
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    if (abilityState_ == SystemAbilityState::NOT_LOADED) {
        return;
    }
    HILOGD(TAG, "[PerformanceTest] SAFWK OnStop systemAbilityId:%{public}d", saId_);
    int64_t begin = GetTickCount();
    std::unordered_map<std::string, std::string> stopReason = LocalAbilityManager::GetInstance().GetStopReason(saId_);
    OnStop(stopReason);
    abilityState_ = SystemAbilityState::NOT_LOADED;
    isRunning_ = false;
    HILOGI(TAG, "[PerformanceTest] SAFWK OnStop systemAbilityId:%{public}d finished, spend:%{public}" PRId64 " ms",
        saId_, (GetTickCount() - begin));
    sptr<ISystemAbilityManager> samgrProxy = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgrProxy == nullptr) {
        HILOGE(TAG, "failed to get samgrProxy");
        return;
    }

    int32_t ret = samgrProxy->RemoveSystemAbility(saId_);
    HILOGI(TAG, "%{public}s to remove ability", (ret == ERR_OK) ? "success" : "failed");
}

void SystemAbility::SADump()
{
    OnDump();
}

int32_t SystemAbility::GetSystemAbilitId() const
{
    return saId_;
}

void SystemAbility::SetLibPath(const std::string& libPath)
{
    libPath_ = libPath;
}

const std::string& SystemAbility::GetLibPath() const
{
    return libPath_;
}

void SystemAbility::SetDependSa(const std::vector<std::int32_t>& dependSa)
{
    dependSa_ = dependSa;
}

const std::vector<std::int32_t>& SystemAbility::GetDependSa() const
{
    return dependSa_;
}

void SystemAbility::SetRunOnCreate(bool isRunOnCreate)
{
    isRunOnCreate_ = isRunOnCreate;
}

bool SystemAbility::IsRunOnCreate() const
{
    return isRunOnCreate_;
}

void SystemAbility::SetDistributed(bool isDistributed)
{
    isDistributed_ = isDistributed;
}

bool SystemAbility::GetDistributed() const
{
    return isDistributed_;
}

bool SystemAbility::GetRunningStatus() const
{
    return isRunning_;
}

SystemAbilityState SystemAbility::GetAbilityState()
{
    std::lock_guard<std::recursive_mutex> autoLock(abilityLock);
    return abilityState_;
}

void SystemAbility::SetDumpLevel(uint32_t dumpLevel)
{
    dumpLevel_ = dumpLevel;
}

uint32_t SystemAbility::GetDumpLevel() const
{
    return dumpLevel_;
}

void SystemAbility::SetDependTimeout(int32_t dependTimeout)
{
    if (dependTimeout >= MIN_DEPENDENCY_TIMEOUT && dependTimeout <= MAX_DEPENDENCY_TIMEOUT) {
        dependTimeout_ = dependTimeout;
    }
    HILOGD(TAG, "new dependTimeout:%{public}d, original dependTimeout:%{public}d", dependTimeout, dependTimeout_);
}

int32_t SystemAbility::GetDependTimeout() const
{
    return dependTimeout_;
}

// The details should be implemented by subclass
void SystemAbility::OnDump()
{
}

// The details should be implemented by subclass
void SystemAbility::OnStart()
{
}

// The details should be implemented by subclass
void SystemAbility::OnStart(const std::unordered_map<std::string, std::string>& startReason)
{
    OnStart();
}

int32_t SystemAbility::OnIdle(const std::unordered_map<std::string, std::string>& idleReason)
{
    HILOGD(TAG, "OnIdle system ability, idle reason %{public}s, %{public}s",
        idleReason.at("eventId").c_str(), idleReason.at("name").c_str());
    return 0;
}

void SystemAbility::OnActive(const std::unordered_map<std::string, std::string>& activeReason)
{
    HILOGD(TAG, "OnActive system ability, active reason %{public}s, %{public}s",
        activeReason.at("eventId").c_str(), activeReason.at("name").c_str());
}

// The details should be implemented by subclass
void SystemAbility::OnStop()
{
}

// The details should be implemented by subclass
void SystemAbility::OnStop(const std::unordered_map<std::string, std::string>& stopReason)
{
    OnStop();
}

// The details should be implemented by subclass
void SystemAbility::OnAddSystemAbility(int32_t systemAbilityId, const std::string& deviceId)
{
}

// The details should be implemented by subclass
void SystemAbility::OnRemoveSystemAbility(int32_t systemAbilityId, const std::string& deviceId)
{
}

sptr<IRemoteObject> SystemAbility::GetSystemAbility(int32_t systemAbilityId)
{
    sptr<ISystemAbilityManager> samgrProxy = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgrProxy == nullptr) {
        HILOGE(TAG, "failed to get samgrProxy");
        return nullptr;
    }

    return samgrProxy->GetSystemAbility(systemAbilityId);
}

void SystemAbility::SetCapability(const std::u16string& capability)
{
    capability_ = capability;
}

const std::u16string& SystemAbility::GetCapability() const
{
    return capability_;
}

void SystemAbility::SetPermission(const std::u16string& permission)
{
    permission_ = permission;
}
}
