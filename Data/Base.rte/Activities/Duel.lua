-- Each human player gets one Ronin soldier with a random weapon. The last player with a soldier standing wins.

function Duel:StartActivity(isNewGame)
	self.BuyMenuEnabled = false;
	if not isNewGame then
		return;
	end

	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			local team = self:GetTeamOfPlayer(player);
			self:SetTeamFunds(0, team);

			local soldier = CreateAHuman("Bandit", "Ronin.rte");
			local weapon = RandomHDFirearm("Weapons - Primary", "Ronin.rte");
			if weapon then
				soldier:AddInventoryItem(weapon);
			end
			-- The players start either side of the middle of the scene, close enough to find each other.
			local startX = SceneMan.SceneWidth * 0.5 + (player % 2 == 0 and -350 or 350) * (1 + math.floor(player / 2));
			soldier.Pos = SceneMan:MovePointToGround(Vector(startX, 0), 0, 0) + Vector(0, -soldier.Radius);
			soldier.Team = team;
			soldier:AddToGroup("Brains");
			MovableMan:AddActor(soldier);

			self:SetPlayerBrain(soldier, player);
			self:SwitchToActor(soldier, player, team);
			self:SetObservationTarget(soldier.Pos, player);
		end
	end
end

function Duel:UpdateActivity()
	if self.ActivityState == Activity.OVER then
		return;
	end

	local playersLeft = 0;
	local lastTeam = Activity.NOTEAM;
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) then
			if MovableMan:IsActor(self:GetPlayerBrain(player)) then
				playersLeft = playersLeft + 1;
				lastTeam = self:GetTeamOfPlayer(player);
			elseif self:GetPlayerBrain(player) then
				self:SetPlayerBrain(nil, player);
				FrameMan:SetScreenText("You lost the duel!", self:ScreenOfPlayer(player), 333, -1, false);
			end
		end
	end

	if playersLeft <= 1 then
		self.WinnerTeam = lastTeam;
		ActivityMan:EndActivity();
	end
end
