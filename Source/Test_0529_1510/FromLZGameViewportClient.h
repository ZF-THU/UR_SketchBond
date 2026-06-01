#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "FromLZGameViewportClient.generated.h"

UCLASS()
class TEST_0529_1510_API UFromLZGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual void Init(struct FWorldContext& WorldContext, UGameInstance* OwningGameInstance, bool bCreateNewAudioDevice = true) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
};
