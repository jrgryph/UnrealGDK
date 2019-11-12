// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "Interop/SpatialSender.h"

#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

#include "Engine/Engine.h"
#include "EngineClasses/SpatialActorChannel.h"
#include "EngineClasses/SpatialNetConnection.h"
#include "EngineClasses/SpatialNetDriver.h"
#include "EngineClasses/SpatialPackageMapClient.h"
#include "EngineClasses/SpatialLoadBalanceEnforcer.h"
#include "Interop/Connection/SpatialWorkerConnection.h"
#include "Interop/SpatialDispatcher.h"
#include "Interop/SpatialReceiver.h"
#include "LoadBalancing/AbstractLBStrategy.h"
#include "Schema/AlwaysRelevant.h"
#include "Schema/AuthorityIntent.h"
#include "Schema/ClientRPCEndpoint.h"
#include "Schema/Heartbeat.h"
#include "Schema/Interest.h"
#include "Schema/RPCPayload.h"
#include "Schema/ServerRPCEndpoint.h"
#include "Schema/Singleton.h"
#include "Schema/SpawnData.h"
#include "Schema/StandardLibrary.h"
#include "Schema/Tombstone.h"
#include "Schema/UnrealMetadata.h"
#include "SpatialConstants.h"
#include "Utils/ActorGroupManager.h"
#include "Utils/ComponentFactory.h"
#include "Utils/InterestFactory.h"
#include "Utils/RepLayoutUtils.h"
#include "Utils/SpatialActorUtils.h"
#include "Utils/SpatialMetrics.h"

DEFINE_LOG_CATEGORY(LogSpatialSender);

using namespace SpatialGDK;

DECLARE_CYCLE_STAT(TEXT("SendComponentUpdates"), STAT_SpatialSenderSendComponentUpdates, STATGROUP_SpatialNet);
DECLARE_CYCLE_STAT(TEXT("ResetOutgoingUpdate"), STAT_SpatialSenderResetOutgoingUpdate, STATGROUP_SpatialNet);
DECLARE_CYCLE_STAT(TEXT("QueueOutgoingUpdate"), STAT_SpatialSenderQueueOutgoingUpdate, STATGROUP_SpatialNet);

FReliableRPCForRetry::FReliableRPCForRetry(UObject* InTargetObject, UFunction* InFunction, Worker_ComponentId InComponentId, Schema_FieldId InRPCIndex, const TArray<uint8>& InPayload, int InRetryIndex)
	: TargetObject(InTargetObject)
	, Function(InFunction)
	, ComponentId(InComponentId)
	, RPCIndex(InRPCIndex)
	, Payload(InPayload)
	, Attempts(1)
	, RetryIndex(InRetryIndex)
{
}

FPendingRPC::FPendingRPC(FPendingRPC&& Other)
	: Offset(Other.Offset)
	, Index(Other.Index)
	, Data(MoveTemp(Other.Data))
	, Entity(Other.Entity)
{
}

void USpatialSender::Init(USpatialNetDriver* InNetDriver, FTimerManager* InTimerManager)
{
	NetDriver = InNetDriver;
	StaticComponentView = InNetDriver->StaticComponentView;
	Connection = InNetDriver->Connection;
	Receiver = InNetDriver->Receiver;
	PackageMap = InNetDriver->PackageMap;
	ClassInfoManager = InNetDriver->ClassInfoManager;
	ActorGroupManager = InNetDriver->ActorGroupManager;
	TimerManager = InTimerManager;

	OutgoingRPCs.BindProcessingFunction(FProcessRPCDelegate::CreateUObject(this, &USpatialSender::SendRPC));
}

Worker_RequestId USpatialSender::CreateEntity(USpatialActorChannel* Channel)
{
	AActor* Actor = Channel->Actor;
	UClass* Class = Actor->GetClass();

	FString ClientWorkerAttribute = GetOwnerWorkerAttribute(Actor);

	WorkerRequirementSet AnyServerRequirementSet;
	WorkerRequirementSet AnyServerOrClientRequirementSet = { SpatialConstants::UnrealClientAttributeSet };

	WorkerAttributeSet OwningClientAttributeSet = { ClientWorkerAttribute };

	WorkerRequirementSet AnyServerOrOwningClientRequirementSet = { OwningClientAttributeSet };
	WorkerRequirementSet OwningClientOnlyRequirementSet = { OwningClientAttributeSet };

	for (const FName& WorkerType : GetDefault<USpatialGDKSettings>()->ServerWorkerTypes)
	{
		WorkerAttributeSet ServerWorkerAttributeSet = { WorkerType.ToString() };

		AnyServerRequirementSet.Add(ServerWorkerAttributeSet);
		AnyServerOrClientRequirementSet.Add(ServerWorkerAttributeSet);
		AnyServerOrOwningClientRequirementSet.Add(ServerWorkerAttributeSet);
	}

	// Add Zoning Attribute if we are using the load balancer.
	const USpatialGDKSettings* SpatialSettings = GetDefault<USpatialGDKSettings>();
	if (SpatialSettings->bEnableUnrealLoadBalancer)
	{
		WorkerAttributeSet ZoningAttributeSet = { SpatialConstants::ZoningAttribute };
		AnyServerRequirementSet.Add(ZoningAttributeSet);
		AnyServerOrClientRequirementSet.Add(ZoningAttributeSet);
		AnyServerOrOwningClientRequirementSet.Add(ZoningAttributeSet);
	}

	WorkerRequirementSet ReadAcl;
	if (Class->HasAnySpatialClassFlags(SPATIALCLASS_ServerOnly))
	{
		ReadAcl = AnyServerRequirementSet;
	}
	else if (Actor->IsA<APlayerController>())
	{
		ReadAcl = AnyServerOrOwningClientRequirementSet;
	}
	else
	{
		ReadAcl = AnyServerOrClientRequirementSet;
	}

	const FClassInfo& Info = ClassInfoManager->GetOrCreateClassInfoByClass(Class);

	const WorkerAttributeSet WorkerAttribute{ Info.WorkerType.ToString() };
	const WorkerRequirementSet AuthoritativeWorkerRequirementSet = { WorkerAttribute };

	WriteAclMap ComponentWriteAcl;
	ComponentWriteAcl.Add(SpatialConstants::POSITION_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	ComponentWriteAcl.Add(SpatialConstants::INTEREST_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	ComponentWriteAcl.Add(SpatialConstants::SPAWN_DATA_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	ComponentWriteAcl.Add(SpatialConstants::SERVER_RPC_ENDPOINT_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	ComponentWriteAcl.Add(SpatialConstants::NETMULTICAST_RPCS_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	ComponentWriteAcl.Add(SpatialConstants::DORMANT_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	ComponentWriteAcl.Add(SpatialConstants::CLIENT_RPC_ENDPOINT_COMPONENT_ID, OwningClientOnlyRequirementSet);

	if (SpatialSettings->bEnableUnrealLoadBalancer)
	{
		const WorkerAttributeSet ACLAttributeSet = { SpatialConstants::ZoningAttribute };
		const WorkerRequirementSet ACLRequirementSet = { ACLAttributeSet };
		ComponentWriteAcl.Add(SpatialConstants::ENTITY_ACL_COMPONENT_ID, ACLRequirementSet);
		ComponentWriteAcl.Add(SpatialConstants::AUTHORITY_INTENT_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	}
	else
	{
		const WorkerAttributeSet ACLAttributeSet = { Info.WorkerType.ToString() };
		const WorkerRequirementSet ACLRequirementSet = { ACLAttributeSet };
		ComponentWriteAcl.Add(SpatialConstants::ENTITY_ACL_COMPONENT_ID, ACLRequirementSet);
	}

	if (Actor->IsNetStartupActor())
	{
		ComponentWriteAcl.Add(SpatialConstants::TOMBSTONE_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
	}

	// If there are pending RPCs, add this component.
	if (OutgoingOnCreateEntityRPCs.Contains(Actor))
	{
		ComponentWriteAcl.Add(SpatialConstants::RPCS_ON_ENTITY_CREATION_ID, AuthoritativeWorkerRequirementSet);
	}

	// If Actor is a PlayerController, add the heartbeat component.
	if (Actor->IsA<APlayerController>())
	{
#if !UE_BUILD_SHIPPING
		ComponentWriteAcl.Add(SpatialConstants::DEBUG_METRICS_COMPONENT_ID, AuthoritativeWorkerRequirementSet);
#endif // !UE_BUILD_SHIPPING
		ComponentWriteAcl.Add(SpatialConstants::HEARTBEAT_COMPONENT_ID, OwningClientOnlyRequirementSet);
	}

	ComponentWriteAcl.Add(SpatialConstants::ALWAYS_RELEVANT_COMPONENT_ID, AuthoritativeWorkerRequirementSet);

	ForAllSchemaComponentTypes([&](ESchemaComponentType Type)
	{
		Worker_ComponentId ComponentId = Info.SchemaComponents[Type];
		if (ComponentId == SpatialConstants::INVALID_COMPONENT_ID)
		{
			return;
		}

		ComponentWriteAcl.Add(ComponentId, AuthoritativeWorkerRequirementSet);
	});

	for (auto& SubobjectInfoPair : Info.SubobjectInfo)
	{
		const FClassInfo& SubobjectInfo = SubobjectInfoPair.Value.Get();

		// Static subobjects aren't guaranteed to exist on actor instances, check they are present before adding write acls
		TWeakObjectPtr<UObject> Subobject = PackageMap->GetObjectFromUnrealObjectRef(FUnrealObjectRef(Channel->GetEntityId(), SubobjectInfoPair.Key));
		if (!Subobject.IsValid())
		{
			continue;
		}

		ForAllSchemaComponentTypes([&](ESchemaComponentType Type)
		{
			Worker_ComponentId ComponentId = SubobjectInfo.SchemaComponents[Type];
			if (ComponentId == SpatialConstants::INVALID_COMPONENT_ID)
			{
				return;
			}

			ComponentWriteAcl.Add(ComponentId, AuthoritativeWorkerRequirementSet);
		});
	}

	// We want to have a stably named ref if this is a loaded Actor.
	// We use this to indicate if a new Actor should be created or to link a pre-existing Actor when receiving an AddEntityOp.
	// Previously, IsFullNameStableForNetworking was used but this was only true if bNetLoadOnClient was true.
	TSchemaOption<FUnrealObjectRef> StablyNamedObjectRef;
	TSchemaOption<bool> bNetStartup;
	if (Actor->HasAnyFlags(RF_WasLoaded) || Actor->bNetStartup)
	{
		// Since we've already received the EntityId for this Actor. It is guaranteed to be resolved
		// with the package map by this point
		FUnrealObjectRef OuterObjectRef = PackageMap->GetUnrealObjectRefFromObject(Actor->GetOuter());
		if (OuterObjectRef == FUnrealObjectRef::UNRESOLVED_OBJECT_REF)
		{
			FNetworkGUID NetGUID = PackageMap->ResolveStablyNamedObject(Actor->GetOuter());
			OuterObjectRef = PackageMap->GetUnrealObjectRefFromNetGUID(NetGUID);
		}

		// No path in SpatialOS should contain a PIE prefix.
		FString TempPath = Actor->GetFName().ToString();
		GEngine->NetworkRemapPath(NetDriver, TempPath, false /*bIsReading*/);

		StablyNamedObjectRef = FUnrealObjectRef(0, 0, TempPath, OuterObjectRef, true);
		bNetStartup = Actor->bNetStartup;
	}

	TArray<Worker_ComponentData> ComponentDatas;
	ComponentDatas.Add(Position(Coordinates::FromFVector(GetActorSpatialPosition(Actor))).CreatePositionData());
	ComponentDatas.Add(Metadata(Class->GetName()).CreateMetadataData());
	ComponentDatas.Add(SpawnData(Actor).CreateSpawnDataData());
	ComponentDatas.Add(UnrealMetadata(StablyNamedObjectRef, ClientWorkerAttribute, Class->GetPathName(), bNetStartup).CreateUnrealMetadataData());

	if (!Class->HasAnySpatialClassFlags(SPATIALCLASS_NotPersistent))
	{
		ComponentDatas.Add(Persistence().CreatePersistenceData());
	}

	if (SpatialSettings->bEnableUnrealLoadBalancer)
	{
		ComponentDatas.Add(AuthorityIntent::CreateAuthorityIntentData(NetDriver->LoadBalanceStrategy->GetLocalVirtualWorkerId()));
	}

	if (RPCsOnEntityCreation* QueuedRPCs = OutgoingOnCreateEntityRPCs.Find(Actor))
	{
		if (QueuedRPCs->HasRPCPayloadData())
		{
			ComponentDatas.Add(QueuedRPCs->CreateRPCPayloadData());
		}
		OutgoingOnCreateEntityRPCs.Remove(Actor);
	}

	if (Class->HasAnySpatialClassFlags(SPATIALCLASS_Singleton))
	{
		ComponentDatas.Add(Singleton().CreateSingletonData());
	}

	if (Actor->bAlwaysRelevant)
	{
		ComponentDatas.Add(AlwaysRelevant().CreateData());
	}

	if (Actor->NetDormancy >= DORM_DormantAll)
	{
		ComponentDatas.Add(Dormant().CreateData());
	}

	// If the Actor was loaded rather than dynamically spawned, associate it with its owning sublevel.
	ComponentDatas.Add(CreateLevelComponentData(Actor));

	if (Actor->IsA<APlayerController>())
	{
#if !UE_BUILD_SHIPPING
		ComponentDatas.Add(ComponentFactory::CreateEmptyComponentData(SpatialConstants::DEBUG_METRICS_COMPONENT_ID));
#endif // !UE_BUILD_SHIPPING
		ComponentDatas.Add(Heartbeat().CreateHeartbeatData());
	}

	ComponentFactory DataFactory(false, NetDriver);

	FRepChangeState InitialRepChanges = Channel->CreateInitialRepChangeState(Actor);
	FHandoverChangeState InitialHandoverChanges = Channel->CreateInitialHandoverChangeState(Info);

	TArray<Worker_ComponentData> DynamicComponentDatas = DataFactory.CreateComponentDatas(Actor, Info, InitialRepChanges, InitialHandoverChanges);
	ComponentDatas.Append(DynamicComponentDatas);

	InterestFactory InterestDataFactory(Actor, Info, NetDriver->ClassInfoManager, NetDriver->PackageMap);
	ComponentDatas.Add(InterestDataFactory.CreateInterestData());

	ComponentDatas.Add(ClientRPCEndpoint().CreateRPCEndpointData());
	ComponentDatas.Add(ServerRPCEndpoint().CreateRPCEndpointData());
	ComponentDatas.Add(ComponentFactory::CreateEmptyComponentData(SpatialConstants::NETMULTICAST_RPCS_COMPONENT_ID));

	// Only add subobjects which are replicating
	for (auto RepSubobject = Channel->ReplicationMap.CreateIterator(); RepSubobject; ++RepSubobject)
	{
		if (UObject* Subobject = RepSubobject.Value()->GetWeakObjectPtr().Get())
		{
			if (Subobject == Actor)
			{
				// Actor's replicator is also contained in ReplicationMap.
				continue;
			}

			// If this object is not in the PackageMap, it has been dynamically created.
			if (!PackageMap->GetUnrealObjectRefFromObject(Subobject).IsValid())
			{
				const FClassInfo* SubobjectInfo = Channel->TryResolveNewDynamicSubobjectAndGetClassInfo(Subobject);

				if (SubobjectInfo == nullptr)
				{
					// This is a failure but there is already a log inside TryResolveNewDynamicSubbojectAndGetClassInfo
					continue;
				}

				ForAllSchemaComponentTypes([&](ESchemaComponentType Type)
				{
					if (SubobjectInfo->SchemaComponents[Type] != SpatialConstants::INVALID_COMPONENT_ID)
					{
						ComponentWriteAcl.Add(SubobjectInfo->SchemaComponents[Type], AuthoritativeWorkerRequirementSet);
					}
				});
			}

			const FClassInfo& SubobjectInfo = ClassInfoManager->GetOrCreateClassInfoByObject(Subobject);

			FRepChangeState SubobjectRepChanges = Channel->CreateInitialRepChangeState(Subobject);
			FHandoverChangeState SubobjectHandoverChanges = Channel->CreateInitialHandoverChangeState(SubobjectInfo);

			TArray<Worker_ComponentData> ActorSubobjectDatas = DataFactory.CreateComponentDatas(Subobject, SubobjectInfo, SubobjectRepChanges, SubobjectHandoverChanges);
			ComponentDatas.Append(ActorSubobjectDatas);
		}
	}

	// Or if the subobject has handover properties, add it as well.
	// NOTE: this is only for subobjects that are a part of the CDO.
	// NOT dynamic subobjects which have been added before entity creation.
	for (auto& SubobjectInfoPair : Info.SubobjectInfo)
	{
		const FClassInfo& SubobjectInfo = SubobjectInfoPair.Value.Get();

		// Static subobjects aren't guaranteed to exist on actor instances, check they are present before adding write acls
		TWeakObjectPtr<UObject> WeakSubobject = PackageMap->GetObjectFromUnrealObjectRef(FUnrealObjectRef(Channel->GetEntityId(), SubobjectInfoPair.Key));
		if (!WeakSubobject.IsValid())
		{
			continue;
		}

		UObject* Subobject = WeakSubobject.Get();

		if (SubobjectInfo.SchemaComponents[SCHEMA_Handover] == SpatialConstants::INVALID_COMPONENT_ID)
		{
			continue;
		}

		// If it contains it, we've already created handover data for it.
		if (Channel->ReplicationMap.Contains(Subobject))
		{
			continue;
		}

		FHandoverChangeState SubobjectHandoverChanges = Channel->CreateInitialHandoverChangeState(SubobjectInfo);

		Worker_ComponentData SubobjectHandoverData = DataFactory.CreateHandoverComponentData(SubobjectInfo.SchemaComponents[SCHEMA_Handover], Subobject, SubobjectInfo, SubobjectHandoverChanges);
		ComponentDatas.Add(SubobjectHandoverData);

		ComponentWriteAcl.Add(SubobjectInfo.SchemaComponents[SCHEMA_Handover], AuthoritativeWorkerRequirementSet);
	}

	ComponentDatas.Add(EntityAcl(ReadAcl, ComponentWriteAcl).CreateEntityAclData());

	Worker_EntityId EntityId = Channel->GetEntityId();
	Worker_RequestId CreateEntityRequestId = Connection->SendCreateEntityRequest(MoveTemp(ComponentDatas), &EntityId);

	return CreateEntityRequestId;
}

Worker_ComponentData USpatialSender::CreateLevelComponentData(AActor* Actor)
{
	UWorld* ActorWorld = Actor->GetTypedOuter<UWorld>();
	if (ActorWorld != NetDriver->World)
	{
		const uint32 ComponentId = ClassInfoManager->GetComponentIdFromLevelPath(ActorWorld->GetOuter()->GetPathName());
		if (ComponentId != SpatialConstants::INVALID_COMPONENT_ID)
		{
			return ComponentFactory::CreateEmptyComponentData(ComponentId);
		}
		else
		{
			UE_LOG(LogSpatialSender, Error, TEXT("Could not find Streaming Level Component for Level %s, processing Actor %s. Have you generated schema?"),
				*ActorWorld->GetOuter()->GetPathName(), *Actor->GetPathName());
		}
	}

	return ComponentFactory::CreateEmptyComponentData(SpatialConstants::NOT_STREAMED_COMPONENT_ID);
}

void USpatialSender::SendAddComponent(USpatialActorChannel* Channel, UObject* Subobject, const FClassInfo& SubobjectInfo)
{
	FRepChangeState SubobjectRepChanges = Channel->CreateInitialRepChangeState(Subobject);
	FHandoverChangeState SubobjectHandoverChanges = Channel->CreateInitialHandoverChangeState(SubobjectInfo);

	ComponentFactory DataFactory(false, NetDriver);

	TArray<Worker_ComponentData> SubobjectDatas = DataFactory.CreateComponentDatas(Subobject, SubobjectInfo, SubobjectRepChanges, SubobjectHandoverChanges);

	for (Worker_ComponentData& ComponentData : SubobjectDatas)
	{
		Connection->SendAddComponent(Channel->GetEntityId(), &ComponentData);
	}

	Channel->PendingDynamicSubobjects.Remove(TWeakObjectPtr<UObject>(Subobject));
}

void USpatialSender::GainAuthorityThenAddComponent(USpatialActorChannel* Channel, UObject* Object, const FClassInfo* Info)
{
	const FClassInfo& ActorInfo = ClassInfoManager->GetOrCreateClassInfoByClass(Channel->Actor->GetClass());
	const WorkerAttributeSet WorkerAttribute{ ActorInfo.WorkerType.ToString() };
	const WorkerRequirementSet AuthoritativeWorkerRequirementSet = { WorkerAttribute };

	EntityAcl* EntityACL = StaticComponentView->GetComponentData<EntityAcl>(Channel->GetEntityId());

	TSharedRef<FPendingSubobjectAttachment> PendingSubobjectAttachment = MakeShared<FPendingSubobjectAttachment>();
	PendingSubobjectAttachment->Subobject = Object;
	PendingSubobjectAttachment->Channel = Channel;
	PendingSubobjectAttachment->Info = Info;

	ForAllSchemaComponentTypes([&](ESchemaComponentType Type)
	{
		Worker_ComponentId ComponentId = Info->SchemaComponents[Type];
		if (ComponentId != SpatialConstants::INVALID_COMPONENT_ID)
		{
			// For each valid ComponentId, we need to wait for its authority delegation before
			// adding the subobject.
			PendingSubobjectAttachment->PendingAuthorityDelegations.Add(ComponentId);
			Receiver->PendingEntitySubobjectDelegations.Add(
				MakeTuple(static_cast<Worker_EntityId_Key>(Channel->GetEntityId()), ComponentId),
				PendingSubobjectAttachment);

			EntityACL->ComponentWriteAcl.Add(Info->SchemaComponents[Type], AuthoritativeWorkerRequirementSet);
		}
	});

	Worker_ComponentUpdate Update = EntityACL->CreateEntityAclUpdate();
	Connection->SendComponentUpdate(Channel->GetEntityId(), &Update);
}

void USpatialSender::SendRemoveComponent(Worker_EntityId EntityId, const FClassInfo& Info)
{
	for (Worker_ComponentId SubobjectComponentId : Info.SchemaComponents)
	{
		if (SubobjectComponentId != SpatialConstants::INVALID_COMPONENT_ID)
		{
			NetDriver->Connection->SendRemoveComponent(EntityId, SubobjectComponentId);
		}
	}

	PackageMap->RemoveSubobject(FUnrealObjectRef(EntityId, Info.SchemaComponents[SCHEMA_Data]));
}

// Creates an entity authoritative on this server worker, ensuring it will be able to receive updates for the GSM.
void USpatialSender::CreateServerWorkerEntity(int AttemptCounter)
{
	const WorkerRequirementSet WorkerIdPermission{ { FString::Format(TEXT("workerId:{0}"), { Connection->GetWorkerId() }) } };

	WriteAclMap ComponentWriteAcl;
	ComponentWriteAcl.Add(SpatialConstants::POSITION_COMPONENT_ID, WorkerIdPermission);
	ComponentWriteAcl.Add(SpatialConstants::METADATA_COMPONENT_ID, WorkerIdPermission);
	ComponentWriteAcl.Add(SpatialConstants::ENTITY_ACL_COMPONENT_ID, WorkerIdPermission);
	ComponentWriteAcl.Add(SpatialConstants::INTEREST_COMPONENT_ID, WorkerIdPermission);

	TArray<Worker_ComponentData> Components;
	Components.Add(Position().CreatePositionData());
	Components.Add(Metadata(FString::Format(TEXT("WorkerEntity:{0}"), { Connection->GetWorkerId() })).CreateMetadataData());
	Components.Add(EntityAcl(WorkerIdPermission, ComponentWriteAcl).CreateEntityAclData());
	Components.Add(InterestFactory::CreateServerWorkerInterest().CreateInterestData());

	Worker_RequestId RequestId = Connection->SendCreateEntityRequest(MoveTemp(Components), nullptr);

	CreateEntityDelegate OnCreateWorkerEntityResponse;
	OnCreateWorkerEntityResponse.BindLambda([WeakSender = TWeakObjectPtr<USpatialSender>(this), AttemptCounter](const Worker_CreateEntityResponseOp& Op)
	{
		if (!WeakSender.IsValid())
		{
			return;
		}
		USpatialSender* Sender = WeakSender.Get();

		if (Op.status_code == WORKER_STATUS_CODE_SUCCESS)
		{
			Sender->NetDriver->WorkerEntityId = Op.entity_id;
			return;
		}

		if (Op.status_code != WORKER_STATUS_CODE_TIMEOUT)
		{
			UE_LOG(LogSpatialSender, Error, TEXT("Worker entity creation request failed: \"%s\""),
				UTF8_TO_TCHAR(Op.message));
			return;
		}

		if (AttemptCounter == SpatialConstants::MAX_NUMBER_COMMAND_ATTEMPTS)
		{
			UE_LOG(LogSpatialSender, Error, TEXT("Worker entity creation request timed out too many times. (%u attempts)"),
				SpatialConstants::MAX_NUMBER_COMMAND_ATTEMPTS);
			return;
		}

		UE_LOG(LogSpatialSender, Warning, TEXT("Worker entity creation request timed out and will retry."));
		FTimerHandle RetryTimer;
		Sender->TimerManager->SetTimer(RetryTimer, [WeakSender, AttemptCounter]()
		{
			if (USpatialSender* Sender = WeakSender.Get())
			{
				Sender->CreateServerWorkerEntity(AttemptCounter + 1);
			}
		}, SpatialConstants::GetCommandRetryWaitTimeSeconds(AttemptCounter), false);
	});

	Receiver->AddCreateEntityDelegate(RequestId, OnCreateWorkerEntityResponse);
}

bool USpatialSender::ValidateOrExit_IsSupportedClass(const FString& PathName)
{
	// Level blueprint classes could have a PIE prefix, this will remove it.
	FString RemappedPathName = PathName;
	GEngine->NetworkRemapPath(NetDriver, RemappedPathName, false);

	return ClassInfoManager->ValidateOrExit_IsSupportedClass(RemappedPathName);
}

void USpatialSender::SendComponentUpdates(UObject* Object, const FClassInfo& Info, USpatialActorChannel* Channel, const FRepChangeState* RepChanges, const FHandoverChangeState* HandoverChanges)
{
	SCOPE_CYCLE_COUNTER(STAT_SpatialSenderSendComponentUpdates);
	Worker_EntityId EntityId = Channel->GetEntityId();

	UE_LOG(LogSpatialSender, Verbose, TEXT("Sending component update (object: %s, entity: %lld)"), *Object->GetName(), EntityId);

	ComponentFactory UpdateFactory(Channel->GetInterestDirty(), NetDriver);

	TArray<Worker_ComponentUpdate> ComponentUpdates = UpdateFactory.CreateComponentUpdates(Object, Info, EntityId, RepChanges, HandoverChanges);

	for (Worker_ComponentUpdate& Update : ComponentUpdates)
	{
		if (!NetDriver->StaticComponentView->HasAuthority(EntityId, Update.component_id))
		{
			UE_LOG(LogSpatialSender, Verbose, TEXT("Trying to send component update but don't have authority! Update will be queued and sent when authority gained. Component Id: %d, entity: %lld"), Update.component_id, EntityId);

			// This is a temporary fix. A task to improve this has been created: UNR-955
			// It may be the case that upon resolving a component, we do not have authority to send the update. In this case, we queue the update, to send upon receiving authority.
			// Note: This will break in a multi-worker context, if we try to create an entity that we don't intend to have authority over. For this reason, this fix is only temporary.
			TArray<Worker_ComponentUpdate>& UpdatesQueuedUntilAuthority = UpdatesQueuedUntilAuthorityMap.FindOrAdd(EntityId);
			UpdatesQueuedUntilAuthority.Add(Update);
			continue;
		}

		Connection->SendComponentUpdate(EntityId, &Update);
	}
}

// Apply (and clean up) any updates queued, due to being sent previously when they didn't have authority.
void USpatialSender::ProcessUpdatesQueuedUntilAuthority(Worker_EntityId EntityId)
{
	if (TArray<Worker_ComponentUpdate>* UpdatesQueuedUntilAuthority = UpdatesQueuedUntilAuthorityMap.Find(EntityId))
	{
		for (Worker_ComponentUpdate& Update : *UpdatesQueuedUntilAuthority)
		{
			Connection->SendComponentUpdate(EntityId, &Update);
		}
		UpdatesQueuedUntilAuthorityMap.Remove(EntityId);
	}
}

void USpatialSender::FlushPackedRPCs()
{
	if (RPCsToPack.Num() == 0)
	{
		return;
	}

	// TODO: This could be further optimized for the case when there's only 1 RPC to be sent during this frame
	// by sending it directly to the corresponding entity, without including the EntityId in the payload - UNR-1563.
	for (const auto& It : RPCsToPack)
	{
		Worker_EntityId PlayerControllerEntityId = It.Key;
		const TArray<FPendingRPC>& PendingRPCArray = It.Value;

		Worker_ComponentUpdate ComponentUpdate = {};

		Worker_ComponentId ComponentId = NetDriver->IsServer() ? SpatialConstants::SERVER_RPC_ENDPOINT_COMPONENT_ID : SpatialConstants::CLIENT_RPC_ENDPOINT_COMPONENT_ID;
		ComponentUpdate.component_id = ComponentId;
		ComponentUpdate.schema_type = Schema_CreateComponentUpdate();
		Schema_Object* EventsObject = Schema_GetComponentUpdateEvents(ComponentUpdate.schema_type);

		for (const FPendingRPC& RPC : PendingRPCArray)
		{
			Schema_Object* EventData = Schema_AddObject(EventsObject, SpatialConstants::UNREAL_RPC_ENDPOINT_PACKED_EVENT_ID);

			Schema_AddUint32(EventData, SpatialConstants::UNREAL_RPC_PAYLOAD_OFFSET_ID, RPC.Offset);
			Schema_AddUint32(EventData, SpatialConstants::UNREAL_RPC_PAYLOAD_RPC_INDEX_ID, RPC.Index);
			SpatialGDK::AddBytesToSchema(EventData, SpatialConstants::UNREAL_RPC_PAYLOAD_RPC_PAYLOAD_ID, RPC.Data.GetData(), RPC.Data.Num());
			Schema_AddEntityId(EventData, SpatialConstants::UNREAL_PACKED_RPC_PAYLOAD_ENTITY_ID, RPC.Entity);
		}

		Connection->SendComponentUpdate(PlayerControllerEntityId, &ComponentUpdate);
	}

	RPCsToPack.Empty();
}

void FillComponentInterests(const FClassInfo& Info, bool bNetOwned, TArray<Worker_InterestOverride>& ComponentInterest)
{
	if (Info.SchemaComponents[SCHEMA_OwnerOnly] != SpatialConstants::INVALID_COMPONENT_ID)
	{
		Worker_InterestOverride SingleClientInterest = { Info.SchemaComponents[SCHEMA_OwnerOnly], bNetOwned };
		ComponentInterest.Add(SingleClientInterest);
	}

	if (Info.SchemaComponents[SCHEMA_Handover] != SpatialConstants::INVALID_COMPONENT_ID)
	{
		Worker_InterestOverride HandoverInterest = { Info.SchemaComponents[SCHEMA_Handover], false };
		ComponentInterest.Add(HandoverInterest);
	}
}

TArray<Worker_InterestOverride> USpatialSender::CreateComponentInterestForActor(USpatialActorChannel* Channel, bool bIsNetOwned)
{
	TArray<Worker_InterestOverride> ComponentInterest;

	const FClassInfo& ActorInfo = ClassInfoManager->GetOrCreateClassInfoByClass(Channel->Actor->GetClass());
	FillComponentInterests(ActorInfo, bIsNetOwned, ComponentInterest);

	// Statically attached subobjects
	for (auto& SubobjectInfoPair : ActorInfo.SubobjectInfo)
	{
		const FClassInfo& SubobjectInfo = SubobjectInfoPair.Value.Get();
		FillComponentInterests(SubobjectInfo, bIsNetOwned, ComponentInterest);
	}

	// Subobjects dynamically created through replication
	for (const auto& Subobject : Channel->CreateSubObjects)
	{
		const FClassInfo& SubobjectInfo = ClassInfoManager->GetOrCreateClassInfoByObject(Subobject);
		FillComponentInterests(SubobjectInfo, bIsNetOwned, ComponentInterest);
	}

	ComponentInterest.Add({ SpatialConstants::CLIENT_RPC_ENDPOINT_COMPONENT_ID, bIsNetOwned });
	ComponentInterest.Add({ SpatialConstants::SERVER_RPC_ENDPOINT_COMPONENT_ID, bIsNetOwned });

	return ComponentInterest;
}

RPCPayload USpatialSender::CreateRPCPayloadFromParams(UObject* TargetObject, const FUnrealObjectRef& TargetObjectRef, UFunction* Function, int ReliableRPCIndex, void* Params)
{
	const FRPCInfo& RPCInfo = ClassInfoManager->GetRPCInfo(TargetObject, Function);

	FSpatialNetBitWriter PayloadWriter = PackRPCDataToSpatialNetBitWriter(Function, Params, ReliableRPCIndex);

	return RPCPayload(TargetObjectRef.Offset, RPCInfo.Index, TArray<uint8>(PayloadWriter.GetData(), PayloadWriter.GetNumBytes()));
}

void USpatialSender::SendComponentInterestForActor(USpatialActorChannel* Channel, Worker_EntityId EntityId, bool bNetOwned)
{
	checkf(!NetDriver->IsServer(), TEXT("Tried to set ComponentInterest on a server-worker. This should never happen!"));

	NetDriver->Connection->SendComponentInterest(EntityId, CreateComponentInterestForActor(Channel, bNetOwned));
}

void USpatialSender::SendComponentInterestForSubobject(const FClassInfo& Info, Worker_EntityId EntityId, bool bNetOwned)
{
	checkf(!NetDriver->IsServer(), TEXT("Tried to set ComponentInterest on a server-worker. This should never happen!"));

	TArray<Worker_InterestOverride> ComponentInterest;
	FillComponentInterests(Info, bNetOwned, ComponentInterest);
	NetDriver->Connection->SendComponentInterest(EntityId, MoveTemp(ComponentInterest));
}

void USpatialSender::SendPositionUpdate(Worker_EntityId EntityId, const FVector& Location)
{
#if !UE_BUILD_SHIPPING
	if (!NetDriver->StaticComponentView->HasAuthority(EntityId, SpatialConstants::POSITION_COMPONENT_ID))
	{
		UE_LOG(LogSpatialSender, Verbose, TEXT("Trying to send Position component update but don't have authority! Update will not be sent. Entity: %lld"), EntityId);
		return;
	}
#endif

	Worker_ComponentUpdate Update = Position::CreatePositionUpdate(Coordinates::FromFVector(Location));
	Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::SendAuthorityIntentUpdate(const AActor& Actor, VirtualWorkerId NewAuthoritativeVirtualWorkerId)
{
	const Worker_EntityId EntityId = PackageMap->GetEntityIdFromObject(&Actor);
	check(EntityId != SpatialConstants::INVALID_ENTITY_ID);
	check(NetDriver->StaticComponentView->GetAuthority(EntityId, SpatialConstants::AUTHORITY_INTENT_COMPONENT_ID));

	UE_LOG(LogSpatialSender, Log, TEXT("(%s) Sending authority update for entity id %d. Virtual worker '%d' should become authoritative over %s"), *NetDriver->Connection->GetWorkerId(), EntityId, NewAuthoritativeVirtualWorkerId, *GetNameSafe(&Actor));

	AuthorityIntent* AuthorityIntentComponent = StaticComponentView->GetComponentData<AuthorityIntent>(EntityId);
	check(AuthorityIntentComponent != nullptr);
	AuthorityIntentComponent->VirtualWorkerId = NewAuthoritativeVirtualWorkerId;

	Worker_ComponentUpdate Update = AuthorityIntentComponent->CreateAuthorityIntentUpdate();
	Connection->SendComponentUpdate(EntityId, &Update);

	if (NetDriver->StaticComponentView->GetAuthority(EntityId, SpatialConstants::ENTITY_ACL_COMPONENT_ID) == WORKER_AUTHORITY_AUTHORITATIVE)
	{
		// Also notify the translator directly on the worker that sends the component update, as the update will short circuit
		NetDriver->LoadBalanceEnforcer->QueueAclAssignmentRequest(EntityId);
	}
}

void USpatialSender::SetAclWriteAuthority(const Worker_EntityId EntityId, const FString& WorkerId)
{
	check(NetDriver);
	if (!NetDriver->StaticComponentView->HasAuthority(EntityId, SpatialConstants::ENTITY_ACL_COMPONENT_ID))
	{
		UE_LOG(LogSpatialLoadBalanceEnforcer, Warning, TEXT("(%s) Failing to set Acl WriteAuth for entity %lld to workerid: %s because this worker doesn't have authority over the EntityACL component."), *NetDriver->Connection->GetWorkerId(), EntityId, *WorkerId);
		return;
	}

	EntityAcl* EntityACL = NetDriver->StaticComponentView->GetComponentData<EntityAcl>(EntityId);
	check(EntityACL);

	const FString& WriteWorkerId = FString::Printf(TEXT("workerId:%s"), *WorkerId);

	WorkerAttributeSet OwningWorkerAttribute = { WriteWorkerId };

	TArray<Worker_ComponentId> ComponentIds;
	EntityACL->ComponentWriteAcl.GetKeys(ComponentIds);

	for (const Worker_ComponentId& ComponentId : ComponentIds)
	{
		if (ComponentId == SpatialConstants::ENTITY_ACL_COMPONENT_ID ||
			ComponentId == SpatialConstants::CLIENT_RPC_ENDPOINT_COMPONENT_ID)
		{
			continue;
		}

		WorkerRequirementSet* RequirementSet = EntityACL->ComponentWriteAcl.Find(ComponentId);
		check(RequirementSet->Num() == 1);
		RequirementSet->Empty();
		RequirementSet->Add(OwningWorkerAttribute);
	}

	UE_LOG(LogSpatialLoadBalanceEnforcer, Log, TEXT("(%s) Setting Acl WriteAuth for entity %lld to workerid: %s"), *NetDriver->Connection->GetWorkerId(), EntityId, *WorkerId);

	Worker_ComponentUpdate Update = EntityACL->CreateEntityAclUpdate();
	NetDriver->Connection->SendComponentUpdate(EntityId, &Update);
}

FRPCErrorInfo USpatialSender::SendRPC(const FPendingRPCParams& Params)
{
	TWeakObjectPtr<UObject> TargetObjectWeakPtr = PackageMap->GetObjectFromUnrealObjectRef(Params.ObjectRef);
	if (!TargetObjectWeakPtr.IsValid())
	{
		// Target object was destroyed before the RPC could be (re)sent
		return FRPCErrorInfo{ nullptr, nullptr, NetDriver->IsServer(), ERPCQueueType::Send, ERPCResult::UnresolvedTargetObject };
	}
	UObject* TargetObject = TargetObjectWeakPtr.Get();

	const FClassInfo& ClassInfo = ClassInfoManager->GetOrCreateClassInfoByObject(TargetObject);
	UFunction* Function = ClassInfo.RPCs[Params.Payload.Index];
	if (Function == nullptr)
	{
		return FRPCErrorInfo{ TargetObject, nullptr, NetDriver->IsServer(), ERPCQueueType::Send, ERPCResult::MissingFunctionInfo };
	}

	ERPCResult Result = SendRPCInternal(TargetObject, Function, Params.Payload);

	return FRPCErrorInfo{ TargetObject, Function, NetDriver->IsServer(), ERPCQueueType::Send, Result };
}

ERPCResult USpatialSender::SendRPCInternal(UObject* TargetObject, UFunction* Function, const RPCPayload& Payload)
{
	USpatialActorChannel* Channel = NetDriver->GetOrCreateSpatialActorChannel(TargetObject);

	if (!Channel)
	{
		UE_LOG(LogSpatialSender, Warning, TEXT("Failed to create an Actor Channel for %s."), *TargetObject->GetName());
		return ERPCResult::NoActorChannel;
	}
	const FRPCInfo& RPCInfo = ClassInfoManager->GetRPCInfo(TargetObject, Function);

	if (Channel->bCreatingNewEntity)
	{
		if (Function->HasAnyFunctionFlags(FUNC_NetClient))
		{
			check(NetDriver->IsServer());

			OutgoingOnCreateEntityRPCs.FindOrAdd(TargetObject).RPCs.Add(Payload);
#if !UE_BUILD_SHIPPING
			NetDriver->SpatialMetrics->TrackSentRPC(Function, RPCInfo.Type, Payload.PayloadData.Num());
#endif // !UE_BUILD_SHIPPING
			return ERPCResult::Success;
		}
	}

	Worker_EntityId EntityId = SpatialConstants::INVALID_ENTITY_ID;

	switch (RPCInfo.Type)
	{
	case SCHEMA_CrossServerRPC:
	{
		Worker_ComponentId ComponentId = SchemaComponentTypeToWorkerComponentId(RPCInfo.Type);

		Worker_CommandRequest CommandRequest = CreateRPCCommandRequest(TargetObject, Payload, ComponentId, RPCInfo.Index, EntityId);

		check(EntityId != SpatialConstants::INVALID_ENTITY_ID);
		Worker_RequestId RequestId = Connection->SendCommandRequest(EntityId, &CommandRequest, SpatialConstants::UNREAL_RPC_ENDPOINT_COMMAND_ID);

#if !UE_BUILD_SHIPPING
		NetDriver->SpatialMetrics->TrackSentRPC(Function, RPCInfo.Type, Payload.PayloadData.Num());
#endif // !UE_BUILD_SHIPPING

		if (Function->HasAnyFunctionFlags(FUNC_NetReliable))
		{
			UE_LOG(LogSpatialSender, Verbose, TEXT("Sending reliable command request (entity: %lld, component: %d, function: %s, attempt: 1)"),
				EntityId, CommandRequest.component_id, *Function->GetName());
			Receiver->AddPendingReliableRPC(RequestId, MakeShared<FReliableRPCForRetry>(TargetObject, Function, ComponentId, RPCInfo.Index, Payload.PayloadData, 0));
		}
		else
		{
			UE_LOG(LogSpatialSender, Verbose, TEXT("Sending unreliable command request (entity: %lld, component: %d, function: %s)"),
				EntityId, CommandRequest.component_id, *Function->GetName());
		}

		return ERPCResult::Success;
	}
	case SCHEMA_NetMulticastRPC:
	case SCHEMA_ClientReliableRPC:
	case SCHEMA_ServerReliableRPC:
	case SCHEMA_ClientUnreliableRPC:
	case SCHEMA_ServerUnreliableRPC:
	{
		FUnrealObjectRef TargetObjectRef = PackageMap->GetUnrealObjectRefFromObject(TargetObject);
		if (TargetObjectRef == FUnrealObjectRef::UNRESOLVED_OBJECT_REF)
		{
			return ERPCResult::UnresolvedTargetObject;
		}

		if (RPCInfo.Type != SCHEMA_NetMulticastRPC && !Channel->IsListening())
		{
			// If the Entity endpoint is not yet ready to receive RPCs -
			// treat the corresponding object as unresolved and queue RPC
			// However, it doesn't matter in case of Multicast
			return ERPCResult::SpatialActorChannelNotListening;
		}

		EntityId = TargetObjectRef.Entity;
		check(EntityId != SpatialConstants::INVALID_ENTITY_ID);

		Worker_ComponentId ComponentId = SchemaComponentTypeToWorkerComponentId(RPCInfo.Type);

		bool bCanPackRPC = GetDefault<USpatialGDKSettings>()->bPackRPCs;
		if (bCanPackRPC && RPCInfo.Type == SCHEMA_NetMulticastRPC)
		{
			bCanPackRPC = false;
		}

		if (bCanPackRPC && GetDefault<USpatialGDKSettings>()->bEnableOffloading)
		{
			if (const AActor* TargetActor = Cast<AActor>(PackageMap->GetObjectFromEntityId(TargetObjectRef.Entity).Get()))
			{
				if (const UNetConnection* OwningConnection = TargetActor->GetNetConnection())
				{
					if (const AActor* ConnectionOwner = OwningConnection->OwningActor)
					{
						if (!ActorGroupManager->IsSameWorkerType(TargetActor, ConnectionOwner))
						{
							UE_LOG(LogSpatialSender, Verbose, TEXT("RPC %s Cannot be packed as TargetActor (%s) and Connection Owner (%s) are on different worker types."),
								*Function->GetName(),
								*TargetActor->GetName(),
								*ConnectionOwner->GetName()
							)
							bCanPackRPC = false;
						}
					}
				}
			}
		}

		if (bCanPackRPC)
		{
			ERPCResult Result = AddPendingRPC(TargetObject, Function, Payload, ComponentId, RPCInfo.Index);
#if !UE_BUILD_SHIPPING
			if (Result == ERPCResult::Success)
			{
				NetDriver->SpatialMetrics->TrackSentRPC(Function, RPCInfo.Type, Payload.PayloadData.Num());
			}
#endif // !UE_BUILD_SHIPPING
			return Result;
		}
		else
		{
			if (!NetDriver->StaticComponentView->HasAuthority(EntityId, ComponentId))
			{
				return ERPCResult::NoAuthority;
			}

			Worker_ComponentUpdate ComponentUpdate = CreateRPCEventUpdate(TargetObject, Payload, ComponentId, RPCInfo.Index);

			Connection->SendComponentUpdate(EntityId, &ComponentUpdate);
#if !UE_BUILD_SHIPPING
			NetDriver->SpatialMetrics->TrackSentRPC(Function, RPCInfo.Type, Payload.PayloadData.Num());
#endif // !UE_BUILD_SHIPPING
			return ERPCResult::Success;
		}
	}
	default:
		checkNoEntry();
		return ERPCResult::InvalidRPCType;
	}
}

void USpatialSender::EnqueueRetryRPC(TSharedRef<FReliableRPCForRetry> RetryRPC)
{
	RetryRPCs.Add(RetryRPC);
}

void USpatialSender::FlushRetryRPCs()
{
	// Retried RPCs are sorted by their index.
	RetryRPCs.Sort([](const TSharedRef<FReliableRPCForRetry>& A, const TSharedRef<FReliableRPCForRetry>& B) { return A->RetryIndex < B->RetryIndex; });
	for (auto& RetryRPC : RetryRPCs)
	{
		RetryReliableRPC(RetryRPC);
	}
	RetryRPCs.Empty();
}

void USpatialSender::RetryReliableRPC(TSharedRef<FReliableRPCForRetry> RetryRPC)
{
	if (!RetryRPC->TargetObject.IsValid())
	{
		// Target object was destroyed before the RPC could be (re)sent
		return;
	}

	UObject* TargetObject = RetryRPC->TargetObject.Get();
	FUnrealObjectRef TargetObjectRef = PackageMap->GetUnrealObjectRefFromObject(TargetObject);
	if (TargetObjectRef == FUnrealObjectRef::UNRESOLVED_OBJECT_REF)
	{
		UE_LOG(LogSpatialSender, Warning, TEXT("Actor %s got unresolved (?) before RPC %s could be retried. This RPC will not be sent."), *TargetObject->GetName(), *RetryRPC->Function->GetName());
		return;
	}

	Worker_CommandRequest CommandRequest = CreateRetryRPCCommandRequest(*RetryRPC, TargetObjectRef.Offset);
	Worker_RequestId RequestId = Connection->SendCommandRequest(TargetObjectRef.Entity, &CommandRequest, SpatialConstants::UNREAL_RPC_ENDPOINT_COMMAND_ID);

	// The number of attempts is used to determine the delay in case the command times out and we need to resend it.
	RetryRPC->Attempts++;
	UE_LOG(LogSpatialSender, Verbose, TEXT("Sending reliable command request (entity: %lld, component: %d, function: %s, attempt: %d)"),
		TargetObjectRef.Entity, RetryRPC->ComponentId, *RetryRPC->Function->GetName(), RetryRPC->Attempts);
	Receiver->AddPendingReliableRPC(RequestId, RetryRPC);
}

void USpatialSender::RegisterChannelForPositionUpdate(USpatialActorChannel* Channel)
{
	ChannelsToUpdatePosition.Add(Channel);
}

void USpatialSender::ProcessPositionUpdates()
{
	for (auto& Channel : ChannelsToUpdatePosition)
	{
		if (Channel.IsValid())
		{
			Channel->UpdateSpatialPosition();
		}
	}

	ChannelsToUpdatePosition.Empty();
}

void USpatialSender::SendCreateEntityRequest(USpatialActorChannel* Channel)
{
	UE_LOG(LogSpatialSender, Log, TEXT("Sending create entity request for %s with EntityId %lld"), *Channel->Actor->GetName(), Channel->GetEntityId());

	Worker_RequestId RequestId = CreateEntity(Channel);
	Receiver->AddPendingActorRequest(RequestId, Channel);
}

void USpatialSender::SendRequestToClearRPCsOnEntityCreation(Worker_EntityId EntityId)
{
	Worker_CommandRequest CommandRequest = RPCsOnEntityCreation::CreateClearFieldsCommandRequest();
	NetDriver->Connection->SendCommandRequest(EntityId, &CommandRequest, SpatialConstants::CLEAR_RPCS_ON_ENTITY_CREATION);
}

void USpatialSender::ClearRPCsOnEntityCreation(Worker_EntityId EntityId)
{
	check(NetDriver->IsServer());
	Worker_ComponentUpdate Update = RPCsOnEntityCreation::CreateClearFieldsUpdate();
	NetDriver->Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::SendClientEndpointReadyUpdate(Worker_EntityId EntityId)
{
	ClientRPCEndpoint Endpoint;
	Endpoint.bReady = true;
	Worker_ComponentUpdate Update = Endpoint.CreateRPCEndpointUpdate();
	NetDriver->Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::SendServerEndpointReadyUpdate(Worker_EntityId EntityId)
{
	ServerRPCEndpoint Endpoint;
	Endpoint.bReady = true;
	Worker_ComponentUpdate Update = Endpoint.CreateRPCEndpointUpdate();
	NetDriver->Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::ProcessOrQueueOutgoingRPC(const FUnrealObjectRef& InTargetObjectRef, SpatialGDK::RPCPayload&& InPayload)
{
	TWeakObjectPtr<UObject> TargetObjectWeakPtr = PackageMap->GetObjectFromUnrealObjectRef(InTargetObjectRef);
	if (!TargetObjectWeakPtr.IsValid())
	{
		// Target object was destroyed before the RPC could be (re)sent
		return;
	}

	UObject* TargetObject = TargetObjectWeakPtr.Get();
	const FClassInfo& ClassInfo = ClassInfoManager->GetOrCreateClassInfoByObject(TargetObject);
	UFunction* Function = ClassInfo.RPCs[InPayload.Index];
	const FRPCInfo& RPCInfo = ClassInfoManager->GetRPCInfo(TargetObject, Function);

	OutgoingRPCs.ProcessOrQueueRPC(InTargetObjectRef, RPCInfo.Type, MoveTemp(InPayload));

	// Try to send all pending RPCs unconditionally
	OutgoingRPCs.ProcessRPCs();
}

FSpatialNetBitWriter USpatialSender::PackRPCDataToSpatialNetBitWriter(UFunction* Function, void* Parameters, int ReliableRPCId) const
{
	FSpatialNetBitWriter PayloadWriter(PackageMap);

	if (GetDefault<USpatialGDKSettings>()->bCheckRPCOrder)
	{
		if (Function->HasAnyFunctionFlags(FUNC_NetReliable) && !Function->HasAnyFunctionFlags(FUNC_NetMulticast))
		{
			PayloadWriter << ReliableRPCId;
		}
	}

	TSharedPtr<FRepLayout> RepLayout = NetDriver->GetFunctionRepLayout(Function);
	RepLayout_SendPropertiesForRPC(*RepLayout, PayloadWriter, Parameters);

	return PayloadWriter;
}

Worker_CommandRequest USpatialSender::CreateRPCCommandRequest(UObject* TargetObject, const RPCPayload& Payload, Worker_ComponentId ComponentId, Schema_FieldId CommandIndex, Worker_EntityId& OutEntityId)
{
	Worker_CommandRequest CommandRequest = {};
	CommandRequest.component_id = ComponentId;
	CommandRequest.command_index = SpatialConstants::UNREAL_RPC_ENDPOINT_COMMAND_ID;
	CommandRequest.schema_type = Schema_CreateCommandRequest();
	Schema_Object* RequestObject = Schema_GetCommandRequestObject(CommandRequest.schema_type);

	FUnrealObjectRef TargetObjectRef(PackageMap->GetUnrealObjectRefFromNetGUID(PackageMap->GetNetGUIDFromObject(TargetObject)));
	ensure(TargetObjectRef != FUnrealObjectRef::UNRESOLVED_OBJECT_REF);

	OutEntityId = TargetObjectRef.Entity;

	RPCPayload::WriteToSchemaObject(RequestObject, TargetObjectRef.Offset, CommandIndex, Payload.PayloadData.GetData(), Payload.PayloadData.Num());

	return CommandRequest;
}

Worker_CommandRequest USpatialSender::CreateRetryRPCCommandRequest(const FReliableRPCForRetry& RPC, uint32 TargetObjectOffset)
{
	Worker_CommandRequest CommandRequest = {};
	CommandRequest.component_id = RPC.ComponentId;
	CommandRequest.command_index = SpatialConstants::UNREAL_RPC_ENDPOINT_COMMAND_ID;
	CommandRequest.schema_type = Schema_CreateCommandRequest();
	Schema_Object* RequestObject = Schema_GetCommandRequestObject(CommandRequest.schema_type);

	RPCPayload::WriteToSchemaObject(RequestObject, TargetObjectOffset, RPC.RPCIndex, RPC.Payload.GetData(), RPC.Payload.Num());

	return CommandRequest;
}

Worker_ComponentUpdate USpatialSender::CreateRPCEventUpdate(UObject* TargetObject, const RPCPayload& Payload, Worker_ComponentId ComponentId, Schema_FieldId EventIndex)
{
	Worker_ComponentUpdate ComponentUpdate = {};

	ComponentUpdate.component_id = ComponentId;
	ComponentUpdate.schema_type = Schema_CreateComponentUpdate();
	Schema_Object* EventsObject = Schema_GetComponentUpdateEvents(ComponentUpdate.schema_type);
	Schema_Object* EventData = Schema_AddObject(EventsObject, SpatialConstants::UNREAL_RPC_ENDPOINT_EVENT_ID);

	FUnrealObjectRef TargetObjectRef(PackageMap->GetUnrealObjectRefFromNetGUID(PackageMap->GetNetGUIDFromObject(TargetObject)));
	ensure(TargetObjectRef != FUnrealObjectRef::UNRESOLVED_OBJECT_REF);

	RPCPayload::WriteToSchemaObject(EventData, Payload.Offset, Payload.Index, Payload.PayloadData.GetData(), Payload.PayloadData.Num());

	return ComponentUpdate;
}
ERPCResult USpatialSender::AddPendingRPC(UObject* TargetObject, UFunction* Function, const RPCPayload& Payload, Worker_ComponentId ComponentId, Schema_FieldId RPCIndex)
{
	FUnrealObjectRef TargetObjectRef(PackageMap->GetUnrealObjectRefFromNetGUID(PackageMap->GetNetGUIDFromObject(TargetObject)));
	ensure(TargetObjectRef != FUnrealObjectRef::UNRESOLVED_OBJECT_REF);

	AActor* TargetActor = Cast<AActor>(PackageMap->GetObjectFromEntityId(TargetObjectRef.Entity).Get());
	check(TargetActor != nullptr);
	UNetConnection* OwningConnection = TargetActor->GetNetConnection();
	if (OwningConnection == nullptr)
	{
		UE_LOG(LogSpatialSender, Warning, TEXT("AddPendingRPC: No connection for object %s (RPC %s, actor %s, entity %lld)"),
			*TargetObject->GetName(), *Function->GetName(), *TargetActor->GetName(), TargetObjectRef.Entity);
		return ERPCResult::NoNetConnection;
	}

	APlayerController* Controller = Cast<APlayerController>(OwningConnection->OwningActor);
	if (Controller == nullptr)
	{
		UE_LOG(LogSpatialSender, Warning, TEXT("AddPendingRPC: Connection's owner is not a player controller for object %s (RPC %s, actor %s, entity %lld): connection owner %s"),
			*TargetObject->GetName(), *Function->GetName(), *TargetActor->GetName(), TargetObjectRef.Entity, *OwningConnection->OwningActor->GetName());
		return ERPCResult::NoOwningController;
	}

	USpatialActorChannel* ControllerChannel = NetDriver->GetOrCreateSpatialActorChannel(Controller);
	if (ControllerChannel == nullptr)
	{
		return ERPCResult::NoControllerChannel;
	}

	if (!ControllerChannel->IsListening())
	{
		return ERPCResult::ControllerChannelNotListening;
	}

	FUnrealObjectRef ControllerObjectRef = PackageMap->GetUnrealObjectRefFromObject(Controller);
	ensure(ControllerObjectRef != FUnrealObjectRef::UNRESOLVED_OBJECT_REF);

	TSet<TWeakObjectPtr<const UObject>> UnresolvedObjects;

	FPendingRPC RPC;
	RPC.Offset = TargetObjectRef.Offset;
	RPC.Index = RPCIndex;
	RPC.Data.SetNumUninitialized(Payload.PayloadData.Num());
	FMemory::Memcpy(RPC.Data.GetData(), Payload.PayloadData.GetData(), Payload.PayloadData.Num());
	RPC.Entity = TargetObjectRef.Entity;
	RPCsToPack.FindOrAdd(ControllerObjectRef.Entity).Emplace(MoveTemp(RPC));
	return ERPCResult::Success;
}

void USpatialSender::SendCommandResponse(Worker_RequestId request_id, Worker_CommandResponse& Response)
{
	Connection->SendCommandResponse(request_id, &Response);
}

void USpatialSender::SendEmptyCommandResponse(Worker_ComponentId ComponentId, Schema_FieldId CommandIndex, Worker_RequestId RequestId)
{
	Worker_CommandResponse Response = {};
	Response.component_id = ComponentId;
	Response.command_index = CommandIndex;
	Response.schema_type = Schema_CreateCommandResponse();

	Connection->SendCommandResponse(RequestId, &Response);
}

// Authority over the ClientRPC Schema component is dictated by the owning connection of a client.
// This function updates the authority of that component as the owning connection can change.
bool USpatialSender::UpdateEntityACLs(Worker_EntityId EntityId, const FString& OwnerWorkerAttribute)
{
	EntityAcl* EntityACL = StaticComponentView->GetComponentData<EntityAcl>(EntityId);

	if (EntityACL == nullptr)
	{
		return false;
	}

	if (!NetDriver->StaticComponentView->HasAuthority(EntityId, SpatialConstants::ENTITY_ACL_COMPONENT_ID))
	{
		UE_LOG(LogSpatialSender, Warning, TEXT("Trying to update EntityACL but don't have authority! Update will not be sent. Entity: %lld"), EntityId);
		return false;
	}

	WorkerAttributeSet OwningClientAttribute = { OwnerWorkerAttribute };
	WorkerRequirementSet OwningClientOnly = { OwningClientAttribute };

	EntityACL->ComponentWriteAcl.Add(SpatialConstants::CLIENT_RPC_ENDPOINT_COMPONENT_ID, OwningClientOnly);
	Worker_ComponentUpdate Update = EntityACL->CreateEntityAclUpdate();

	Connection->SendComponentUpdate(EntityId, &Update);
	return true;
}

void USpatialSender::UpdateInterestComponent(AActor* Actor)
{
	Worker_EntityId EntityId = PackageMap->GetEntityIdFromObject(Actor);
	if (EntityId == SpatialConstants::INVALID_ENTITY_ID)
	{
		UE_LOG(LogSpatialSender, Verbose, TEXT("Attempted to update interest for non replicated actor: %s"), *Actor->GetName());
		return;
	}

	InterestFactory InterestUpdateFactory(Actor, ClassInfoManager->GetOrCreateClassInfoByObject(Actor), NetDriver->ClassInfoManager, NetDriver->PackageMap);
	Worker_ComponentUpdate Update = InterestUpdateFactory.CreateInterestUpdate();

	Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::RetireEntity(const Worker_EntityId EntityId)
{
	if (AActor* Actor = Cast<AActor>(PackageMap->GetObjectFromEntityId(EntityId).Get()))
	{
		if (Actor->IsNetStartupActor())
		{
			check(StaticComponentView->HasComponent(EntityId, SpatialConstants::TOMBSTONE_COMPONENT_ID) == false);
			// In the case that this is a startup actor, we won't actually delete the entity in SpatialOS.  Instead we'll Tombstone it.
			Receiver->RemoveActor(EntityId);
			AddTombstoneToEntity(EntityId);
		}
		else
		{
			Connection->SendDeleteEntityRequest(EntityId);
		}
	}
	else
	{
		UE_LOG(LogSpatialSender, Warning, TEXT("RetireEntity: Couldn't get Actor from PackageMap for EntityId: %lld"), EntityId);
	}
}

void USpatialSender::AddTombstoneToEntity(const Worker_EntityId EntityId)
{
	check(NetDriver->StaticComponentView->HasAuthority(EntityId, SpatialConstants::TOMBSTONE_COMPONENT_ID));

	Worker_AddComponentOp AddComponentOp{};
	AddComponentOp.entity_id = EntityId;
	AddComponentOp.data = Tombstone().CreateData();
	Connection->SendAddComponent(EntityId, &AddComponentOp.data);
	StaticComponentView->OnAddComponent(AddComponentOp);

#if WITH_EDITOR
	NetDriver->TrackTombstone(EntityId);
#endif
}
