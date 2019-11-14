// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"

#include <WorkerSDK/improbable/c_worker.h>

#include "SpatialLoadBalanceEnforcer.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpatialLoadBalanceEnforcer, Log, All)

class SpatialVirtualWorkerTranslator;
class USpatialSender;
class USpatialStaticComponentView;

#pragma optimize("", off)

//struct LockedActors
//{
//public:
//	bool IsActorLocked(const AActor* Actor)
//	{
//		return (LockedActors.Contains(Actor));
//	}
//
//	void LockActor(const AActor* Actor)
//	{
//		LockedActors.Add(Actor);
//	}
//
//	void UnlockActor(const AActor* Actor)
//	{
//		LockedActors.Remove(Actor);
//	}
//
//private:
//	TSet<const AActor*> LockedActors;
//};
//

using ScopedLockHandle = TSharedPtr<uint8_t>;

struct LockedActorsContainer
{
public:
	bool IsActorLocked(const AActor* Actor)
	{
		if (ScopedLockHandle* LockedActor = LockedActors.Find(Actor))
		{
			check(LockedActor->IsValid());

			if (LockedActor->GetSharedReferenceCount() == 1)
			{
				LockedActors.Remove(Actor);
				return false;
			}
			else
			{
				return true;
			}
		}
		else
		{
			return false;
		}
	}

	ScopedLockHandle LockActor(const AActor* Actor)
	{
		ScopedLockHandle& LockedActor = LockedActors.FindOrAdd(Actor);
		LockedActor = MakeShared<unsigned char>(0);
		return LockedActor;
	}

private:
	TMap<const AActor*, ScopedLockHandle> LockedActors;
};

UCLASS()
class USpatialLoadBalanceEnforcer : public UObject
{
	GENERATED_UCLASS_BODY()

public:

	void Init(const FString &InWorkerId, USpatialStaticComponentView* InStaticComponentView, USpatialSender* InSpatialSender, SpatialVirtualWorkerTranslator* InVirtualWorkerTranslator);
	void Tick();

	void AuthorityChanged(const Worker_AuthorityChangeOp& AuthOp);
	void QueueAclAssignmentRequest(const Worker_EntityId EntityId);

	void OnAuthorityIntentComponentUpdated(const Worker_ComponentUpdateOp& Op);

	LockedActorsContainer LockedActors;
private:

	FString WorkerId;
	USpatialStaticComponentView* StaticComponentView;
	USpatialSender* Sender;
	SpatialVirtualWorkerTranslator* VirtualWorkerTranslator;

	struct WriteAuthAssignmentRequest
	{
		WriteAuthAssignmentRequest(Worker_EntityId InputEntityId)
			: EntityId(InputEntityId)
			, ProcessAttempts(0)
		{}
		Worker_EntityId EntityId;
		int32 ProcessAttempts;
	};

	TArray<WriteAuthAssignmentRequest> AclWriteAuthAssignmentRequests;

	void ProcessQueuedAclAssignmentRequests();
};

#pragma optimize("", on)
