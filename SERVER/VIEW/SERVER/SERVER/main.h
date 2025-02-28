#pragma once

#include "SESSION.h"
#include "MemoryPool.h"
#include "MapObject.h"
#include "Monster.h"

// 게임 로직 이벤트 타입
enum EVENT_TYPE { EV_MONSTER_UPDATE };

// DB I/O 이벤트 타입
enum DB_EVENT_TYPE { EV_SIGNIN, EV_SIGNUP, EV_SAVE, EV_RESET };

// 타이머 이벤트
struct TIMER_EVENT {
	int room_id;	// 이벤트가 발생하는 방 ID
	int obj_id;		// 이벤트와 관련된 객체 ID
	high_resolution_clock::time_point wakeup_time;	// 이벤트가 발생할 시간
	EVENT_TYPE event_id; 
	constexpr bool operator < (const TIMER_EVENT& _Left) const // 우선순위 큐에서 타이머 이벤트를 wakeup_time 기준으로 정렬하기 위해 < 연산자 오버로딩
	{
		return (wakeup_time > _Left.wakeup_time);
	}
};

// DB 이벤트
struct DB_EVENT {
	unsigned short session_id = -1; // 해당 작업과 연관된 세션 ID
	wchar_t user_id[IDPW_SIZE]{}; // 사용자 계정 정보
	wchar_t user_password[IDPW_SIZE]{};
	DB_EVENT_TYPE _event;
};

// 맵의 그리드의 크기
constexpr int GRID_SIZE_X = 150;  
constexpr int GRID_SIZE_Y = 2;
constexpr int GRID_SIZE_Z = 1200;

constexpr int CELL_SIZE = 4; // 각 그리드 셀의 크기

// 맵의 장애물 정보를 저장하는 3차원 배열
array<array<array<bool, GRID_SIZE_Z>, GRID_SIZE_X>,GRID_SIZE_Y> ObstacleGrid = { false };

// 타이머 기반 이벤트 처리
concurrent_priority_queue<TIMER_EVENT> timer_queue;
// 데이터베이스 작업 처리
concurrent_queue<DB_EVENT> db_queue;

array<array<SESSION, MAX_USER_PER_ROOM>, MAX_ROOM> clients;
array<array<Monster*, MONSTER_PER_STAGE* STAGE_NUMBERS>, MAX_ROOM> monsters;

vector<MonsterInfo> StagesInfo;

array<vector<MapObject*>, OBJECT_ARRAY_SIZE> Obstacles;
array<vector<Key_Object>, 7> Key_Items;

SESSION* getClient(int c_id)
{
	try {
		return &clients[c_id / MAX_USER_PER_ROOM][c_id % MAX_USER_PER_ROOM];
	}
	catch (const exception& e) {
		cout << "getClient catched error -" << e.what() << endl;
		return nullptr;
	}
}

array<SESSION, MAX_USER_PER_ROOM>* getRoom_Clients(int c_id)
{
	try {
		return &clients[c_id / MAX_USER_PER_ROOM];
	}
	catch (const exception& e) {
		cout << "getRoom_Clients catched error -" << e.what() << endl;
		return nullptr;
	}
}

array<Monster*, MONSTER_PER_STAGE* STAGE_NUMBERS>* getRoom_Monsters(int c_id)
{
	try {
		return &monsters[c_id / MAX_USER_PER_ROOM];
	}
	catch (const exception& e) {
		cout << "getRoom_Monsters catched error -" << e.what() << endl;
		return nullptr;
	}
}

vector<MapObject*>* getRoom_Obstacles(XMFLOAT3 Pos)
{
	try {
		return &Obstacles[static_cast<int>(Pos.z) / STAGE_SIZE];
	}
	catch (const exception& e) {
		cout << "getRoom_Obstacles catched error -" << e.what() << endl;
		return nullptr;
	}
}

short get_remain_Monsters(int c_id)
{
	short cnt = 0;
	auto room_Monsters = getRoom_Monsters(c_id);
	if (room_Monsters == nullptr) {
		cout << "wrong session_id - get_remain_Monsters" << endl;
	}
	else {
		for (auto& monster : *room_Monsters) {
			if (monster->alive) cnt++;
		}
	}
	return cnt;
}


void disconnect(int c_id)
{
	bool game_in_progress = false;
	auto Monsters = getRoom_Monsters(c_id);
	auto Room_Clients = getRoom_Clients(c_id);
	if (Room_Clients == nullptr || Monsters == nullptr) {
		cout << "wrong session_id - disconnect" << endl;
		return;
	}
	for (auto& pl : *Room_Clients) {

		if ((ST_INGAME != pl._state.load() && ST_DEAD != pl._state.load()) || pl._id == c_id) continue;

		pl.send_remove_player_packet(c_id);
		game_in_progress = true;
	}
	SESSION* CL = getClient(c_id);
	if (CL == nullptr) {
		cout << "wrong session_id - disconnect" << endl;
		return;
	}
	closesocket(CL->_socket);

	if (game_in_progress) {
		CL->_state.store(ST_CRASHED);
	}

	else {
		for (auto& pl : *Room_Clients) {
			if (pl._state.load() == ST_FREE) continue;
			pl._state = ST_FREE;
		}

		for (auto& mon : *getRoom_Monsters(c_id)) {
			mon->Re_Initialize(StagesInfo[mon->m_id].type, StagesInfo[mon->m_id].Pos);
		}
	}
}


void Summon_Monster(int roomNum, int stageNum)
{
	for (int j = 0; j < MAX_USER_PER_ROOM; ++j) {
		clients[roomNum][j].cur_stage.store(stageNum);
	}

	for (int i = (stageNum - 1) * MONSTER_PER_STAGE; i < stageNum * MONSTER_PER_STAGE; ++i) {
		bool old_state = false;
		if (false == atomic_compare_exchange_strong(&monsters[roomNum][i]->alive, &old_state, true))
			continue;
		for (int j = 0; j < MAX_USER_PER_ROOM; ++j) {
			if (clients[roomNum][j]._state.load() == ST_INGAME || clients[roomNum][j]._state.load() == ST_DEAD) {
				clients[roomNum][j].send_summon_monster_packet(monsters[roomNum][i]);
			}
		}
		TIMER_EVENT ev{ roomNum, monsters[roomNum][i]->m_id, high_resolution_clock::now(), EV_MONSTER_UPDATE };
		timer_queue.push(ev);
	}
}

void SESSION::CheckPosition(XMFLOAT3 newPos)
{

	XMFLOAT3 newCenter = Vector3::Add(newPos, XMFLOAT3(0,10.f,0));			
	try {
		for (const auto& object : Obstacles.at(static_cast<int>(newCenter.z) / AREA_SIZE)) {	
			if (object->m_xmOOBB.Contains(XMLoadFloat3(&newCenter))) {
				SetVelocity(XMFLOAT3{ 0,0,0 });
				return;
			}
		}
	}
	catch (const exception& e) {
		cout << "checkPosition catched error -" << e.what() << endl;
		disconnect(_id);
		return;
	}


	lock_guard<mutex> player_lock{ _s_lock };
	SetPosition(newPos);
	UpdateBoundingBox();
	short stage = 0;
	if (GetPosition().y >= -100.f)	
		stage = static_cast<short>((GetPosition().z - 300.f) / STAGE_SIZE);
	else
		stage = 3 + static_cast<short>((MAP_Z_SIZE - GetPosition().z) / STAGE_SIZE);

	if (stage == 6 && GetPosition().z < 400.f && get_remain_Monsters(_id) == 0) {
		auto Room_Clients = getRoom_Clients(_id);
		if (Room_Clients == nullptr) {
			cout << "wrong session_id - processing game clear" << endl;
			return;
		}
		for (auto& cl : *Room_Clients) {
			if (cl._state.load() != ST_INGAME) continue;
			cl.send_clear_packet();
		}
		return;
	}

	if (stage > cur_stage.load()) {
		Summon_Monster(_id / MAX_USER_PER_ROOM, stage);
	}
}

int get_new_client_id()
{
	for (int i = 0; i < MAX_ROOM; ++i) {
		for (int j = 0; j < MAX_USER_PER_ROOM; ++j) {
			if (clients[i][j]._state.load() == ST_FREE)
				return i * MAX_USER_PER_ROOM + j;
		}
	}
	return -1;
}



void SESSION::Update(CS_HEARTBEAT_PACKET* packet)
{
	direction.store(packet->direction);
	SetVelocity(packet->vel);
	CheckPosition(packet->pos);
#ifdef _STRESS_TEST
	recent_updateTime = packet->move_time;
#endif
}




short Heuristic(const XMFLOAT3& start, const XMFLOAT3& dest) {
	return abs(dest.z - start.z) + abs(dest.x - start.x);
}


bool Monster::check_path(const XMFLOAT3& _pos, unordered_set<XMFLOAT3, PointHash, PointEqual>& CloseList, BoundingBox& check_box)
{
	int collide_range_z = static_cast<int>(_pos.z / AREA_SIZE);
	check_box.Center = _pos;

	if (CloseList.count(_pos)) {
		return false;
	}

	for (auto& monster : monsters[room_num]) {
		if (monster->alive.load() == false || monster->m_id == m_id) continue;
		if (monster->BB.Intersects(check_box))
			return false;
	}

	try {
		if (m_id < 20) {
			if (ObstacleGrid[0].at(int(_pos.x) / CELL_SIZE).at(int(_pos.z) / CELL_SIZE) == false)
				return false;
		}
		else {
			if (ObstacleGrid[1].at(int(_pos.x) / CELL_SIZE).at(int(_pos.z) / CELL_SIZE) == false)
				return false;
		}
	}
	catch (const exception& e) {
		//cout << "array access error - " << _pos.x << ", " << _pos.z << endl;
		return false;
	}

	return true;
}

float nx[8]{ -1,1,0,0, -1, -1, 1, 1 };
float nz[8]{ 0,0,1,-1, -1, 1, -1, 1 };
XMFLOAT3 Monster::Find_Direction(float fTimeElapsed, XMFLOAT3 start, XMFLOAT3 dest)
{
	if (Vector3::Compare(start, dest)) return Pos;

	auto start_time = high_resolution_clock::now();
	unordered_set<XMFLOAT3, PointHash, PointEqual> closelist{};
	unordered_map<XMFLOAT3, shared_ptr<A_star_Node>, PointHash, PointEqual> openlist;
	priority_queue<shared_ptr<A_star_Node>, vector<shared_ptr<A_star_Node>>, CompareNodes> pq;

	shared_ptr<A_star_Node> startNode = make_shared<A_star_Node>(start, 0.f, Heuristic(start, dest), nullptr);
	openlist.insert(make_pair(start, startNode));
	pq.push(startNode);

	BoundingBox CheckBox = BoundingBox(start, BB.Extents);
	while (!pq.empty())
	{
		if (duration_cast<milliseconds>(high_resolution_clock::now() - start_time).count() > 3) break;
		shared_ptr<A_star_Node> S_Node = pq.top();
		pq.pop();

		if (Vector3::Length(Vector3::Subtract(dest, S_Node->Pos)) < min(attack_range,MELEE_ATTACK_RANGE))
		{
			while (S_Node->parent != nullptr)
			{
				if (Vector3::Compare(S_Node->parent->Pos, start))
				{
					XMFLOAT3 next_pos = S_Node->Pos;
					return next_pos;
				}
				S_Node = S_Node->parent;
			}
		}

		closelist.insert(S_Node->Pos);

		for (int i = 0; i < 8; i++) {
			XMFLOAT3 near_pos = Vector3::Add(S_Node->Pos, Vector3::ScalarProduct(XMFLOAT3{ nx[i],0,nz[i] }, speed * fTimeElapsed, false));

			if (check_path(near_pos, closelist, CheckBox) == false)
				continue;

			float _G = S_Node->G + speed * fTimeElapsed * sqrt(abs(nx[i]) + abs(nz[i]));

			auto neighborIter = openlist.find(near_pos);
			if (neighborIter == openlist.end()) {
				float neighborH = Heuristic(near_pos, dest);
				shared_ptr<A_star_Node> neighborNode = make_shared<A_star_Node>(near_pos, _G, neighborH, S_Node);
				openlist.insert(make_pair(near_pos, neighborNode));
				pq.push(neighborNode);
			}
			else if (_G < neighborIter->second->G)
			{
				float neighborH = Heuristic(near_pos, dest);
				neighborIter->second->G = _G;
				neighborIter->second->H = neighborH;
				neighborIter->second->F = _G + neighborH;
				neighborIter->second->parent = S_Node;
			}
		}
	}
	wander = true;
	return Pos;
}

int Monster::get_targetID()
{
	for (int i = 0; i < MAX_USER_PER_ROOM; ++i) {
		if (clients[room_num][i]._state.load() != ST_INGAME) {
			distances[i].distance = view_range;
			distances[i]._id = -1;
			continue;
		}
		float distance_z = clients[room_num][i].GetPosition().z - Pos.z;
		float distance_x = clients[room_num][i].GetPosition().x - Pos.x;
		distances[i].distance = sqrtf(distance_z * distance_z + distance_x * distance_x);
		distances[i]._id = clients[room_num][i]._id;
	}

	auto min = (*min_element(distances.begin(), distances.end(), 
		[](const Player_Distance& a, const Player_Distance& b) {
		return a.distance < b.distance;
		}));
	
	if (min.distance < view_range)
	{
		return min._id;
	}
	else return -1;
}

void Monster::Update(float fTimeElapsed)
{
	switch (GetState())
	{
	case NPC_State::Idle: {
		target_id = get_targetID();
		if (target_id != -1) {
			const auto& targetPlayer = &clients[room_num][target_id];
			XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
			g_distance = Vector3::XZLength(distanceVector);
			if (attack_range >= g_distance)
			{
				SetState(NPC_State::Attack);
			}
			else {
				SetState(NPC_State::Chase);
			}
		}
	}
						break;
	case NPC_State::Chase: {
		if (wander) {
			while (true) {
				short rand_dir = rand() % 8;
				XMFLOAT3 dir = XMFLOAT3(nx[rand_dir], 0, nz[rand_dir]);
				XMFLOAT3 newPos = Vector3::Add(Pos, Vector3::ScalarProduct(dir, speed * fTimeElapsed, false));

				if (m_id < 20) {
					if (ObstacleGrid[0].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
						continue;
				}
				else {
					if (ObstacleGrid[1].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
						continue;
				}


				for (const auto& monster : monsters[room_num]) {
					if (monster->alive.load() == false || monster->m_id == m_id) continue;
					if (monster->BB.Intersects(BoundingBox(newPos, BB.Extents))) {
						continue;
					}
				}


				Pos = newPos;
				BB.Center = Pos;
				break;
			}
			wander_timer += fTimeElapsed;
			if (wander_timer > 2.f) {
				wander_timer = 0.f;
				wander = false;
				SetState(NPC_State::Idle);
			}
		}
		else {
			const auto& targetPlayer = &clients[room_num][target_id];
			XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
			g_distance = Vector3::XZLength(distanceVector);

			if (clients[room_num][target_id]._state.load() != ST_INGAME)
			{
				SetState(NPC_State::Idle);
				target_id = -1;
				break;
			}
			if (attack_range >= g_distance)
			{
				SetState(NPC_State::Attack);
				break;
			}
			const int collide_range = static_cast<int>(Pos.z / AREA_SIZE);
			XMFLOAT3 vel = Vector3::XZNormalize(distanceVector);
			XMFLOAT3 newPos = Vector3::Add(Pos, Vector3::ScalarProduct(vel, speed * fTimeElapsed, false));
			bool collide = false;

			if (m_id < 20) {
				if (ObstacleGrid[0].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
					collide = true;
			}
			else {
				if (ObstacleGrid[1].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
					collide = true;
			}

			if (collide == false) {
				for (const auto& monster : monsters[room_num]) {
					if (monster->alive.load() == false || monster->m_id == m_id) continue;
					if (monster->BB.Intersects(BoundingBox(newPos, BB.Extents))) {
						collide = true;
						break;
					}
				}
			}

			if (collide == true) {
				Pos = Find_Direction(fTimeElapsed, Pos, targetPlayer->GetPosition());
				BB.Center = Pos;
			}
			else {
				Pos = newPos;
				BB.Center = Pos;
			}
		}
	}
						 break;
	case NPC_State::Attack: {
		const auto& targetPlayer = &clients[room_num][target_id];
		XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
		g_distance = Vector3::Length(distanceVector);

		attack_timer -= fTimeElapsed;
		if (targetPlayer->_state != ST_INGAME) {
			SetState(NPC_State::Idle);
			target_id = -1;
			SetAttackTimer(attack_cycle);
			break;
		}
		if (attacked == false && GetAttackTimer() <= attack_cycle / 2.f) {
			if (attack_range > g_distance) {
				lock_guard <mutex> ll{ targetPlayer->_s_lock };
				targetPlayer->HP -= GetPower();
				if (targetPlayer->HP <= 0) {
					targetPlayer->_state.store(ST_DEAD);
					for (auto& cl : clients[room_num]) {
						if (cl._state.load() == ST_INGAME || cl._state.load() == ST_DEAD)
							cl.send_update_packet(targetPlayer);
					}
				}
			}
			attacked = true;
			break;
		}
		if (GetAttackTimer() <= 0) {
			if (attack_range <= g_distance)
			{
				SetState(NPC_State::Chase);
				SetAttackTimer(attack_cycle);
			}
			SetAttackTimer(attack_cycle);
			attacked = false;
		}
	}
						  break;
	case NPC_State::Dead: {
		dead_timer -= fTimeElapsed;
		if (dead_timer <= 0) {		
			alive.store(false);
		}
	}
						break;
	default:
		break;
	}
}

void SorcererMonster::Update(float fTimeElapsed)
{
	if (Vector3::Length(MagicLook) > 0.f) {
		MagicPos = Vector3::Add(MagicPos, Vector3::ScalarProduct(MagicLook, 100.f * fTimeElapsed, false)); // HAT_SPEED = 200.f
		for (auto& player : clients[room_num]) {
			lock_guard <mutex> ll{ player._s_lock };
			if (BoundingBox(MagicPos, BULLET_SIZE).Intersects(player.m_xmOOBB))
			{
				player.HP -= GetPower();
				MagicPos.x = 5000;
				MagicLook.x = MagicLook.y = MagicLook.z = 0.f;
				if (player.HP <= 0)
				{
					player._state.store(ST_DEAD);
					for (auto& cl : clients[room_num]) {
						if (cl._state.load() == ST_INGAME || cl._state.load() == ST_DEAD)
							cl.send_update_packet(&player);
					}
				}
			}
		}
		try {
			for (auto& obj : Obstacles.at(static_cast<int>(MagicPos.z) / AREA_SIZE)) {
				if (obj->m_xmOOBB.Contains(XMLoadFloat3(&MagicPos))) {
					MagicPos.x = 5000;
					MagicLook.x = MagicLook.y = MagicLook.z = 0.f;
				}

			}
		}
		catch (const exception& e) {
			//cout << "Hat Update catched error -" << e.what() << endl;
			MagicPos = Pos;
			return;
		}
	}
	switch (GetState())
	{
	case NPC_State::Idle: {
		target_id = get_targetID();
		if (target_id != -1) {
			const auto& targetPlayer = &clients[room_num][target_id];
			XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
			g_distance = Vector3::XZLength(distanceVector);
			if (attack_range >= g_distance)
			{
				SetState(NPC_State::Attack);
			}
			else {
				SetState(NPC_State::Chase);
			}
		}
	}
						break;
	case NPC_State::Chase: {
		if (wander) {
			while (true) {
				short rand_dir = rand() % 8;
				XMFLOAT3 dir = XMFLOAT3(nx[rand_dir], 0, nz[rand_dir]);
				XMFLOAT3 newPos = Vector3::Add(Pos, Vector3::ScalarProduct(dir, speed * fTimeElapsed, false));

				if (m_id < 20) {
					if (ObstacleGrid[0].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
						continue;
				}
				else {
					if (ObstacleGrid[1].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
						continue;
				}


				for (const auto& monster : monsters[room_num]) {
					if (monster->alive.load() == false || monster->m_id == m_id) continue;
					if (monster->BB.Intersects(BoundingBox(newPos, BB.Extents))) {
						continue;
					}
				}


				Pos = newPos;
				BB.Center = Pos;
				break;
			}
			wander_timer += fTimeElapsed;
			if (wander_timer > 2.f) {
				wander_timer = 0.f;
				wander = false;
				SetState(NPC_State::Idle);
			}
		}
		else {
			const auto& targetPlayer = &clients[room_num][target_id];
			XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
			g_distance = Vector3::XZLength(distanceVector);

			if (clients[room_num][target_id]._state.load() != ST_INGAME)
			{
				SetState(NPC_State::Idle);
				target_id = -1;
				break;
			}
			if (attack_range >= g_distance)
			{
				SetState(NPC_State::Attack);
				break;
			}
			const int collide_range = static_cast<int>(Pos.z / AREA_SIZE);
			XMFLOAT3 vel = Vector3::XZNormalize(distanceVector);
			XMFLOAT3 newPos = Vector3::Add(Pos, Vector3::ScalarProduct(vel, speed * fTimeElapsed, false));
			bool collide = false;

			if (m_id < 20) {
				if (ObstacleGrid[0].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
					collide = true;
			}
			else {
				if (ObstacleGrid[1].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
					collide = true;
			}

			if (collide == false) {
				for (const auto& monster : monsters[room_num]) {
					if (monster->alive.load() == false || monster->m_id == m_id) continue;
					if (monster->BB.Intersects(BoundingBox(newPos, BB.Extents))) {
						collide = true;
						break;
					}
				}
			}

			if (collide == true) {
				Pos = Find_Direction(fTimeElapsed, Pos, targetPlayer->GetPosition());
				BB.Center = Pos;
			}
			else {
				Pos = newPos;
				BB.Center = Pos;
			}
		}
	}
						 break;
	case NPC_State::Attack: {
		const auto& targetPlayer = &clients[room_num][target_id];
		XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
		g_distance = Vector3::Length(distanceVector);

		attack_timer -= fTimeElapsed;
		if (targetPlayer->_state != ST_INGAME) {
			SetState(NPC_State::Idle);
			target_id = -1;
			SetAttackTimer(attack_cycle);
			break;
		}
		if (attacked == false && GetAttackTimer() <= attack_cycle / 2.f) {
			MagicPos = Vector3::Add(GetPosition(), XMFLOAT3(0, 10, 0));
			MagicLook = Vector3::Normalize(distanceVector);
			attacked = true;
			break;
		}
		if (GetAttackTimer() <= 0) {
			if (attack_range <= g_distance)
			{
				SetState(NPC_State::Chase);
				SetAttackTimer(attack_cycle);
			}
			SetAttackTimer(attack_cycle);
			attacked = false;
		}
	}
						  break;
	case NPC_State::Dead: {
		dead_timer -= fTimeElapsed;
		if (dead_timer <= 0) {
			alive.store(false);
		}
	}
						break;
	default:
		break;
	}
}

void BossMonster::Update(float fTimeElapsed)
{
	switch (GetState())
	{
	case NPC_State::Idle: {
		target_id = get_targetID();
		if (target_id != -1) {
			const auto& targetPlayer = &clients[room_num][target_id];
			XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
			g_distance = Vector3::XZLength(distanceVector);
			if (attack_range >= g_distance)
			{
				SetState(NPC_State::Attack);
			}
			else {
				SetState(NPC_State::Chase);
			}
		}
	}
						break;
	case NPC_State::Chase: {
		if (wander) {
			while (true) {
				short rand_dir = rand() % 8;
				XMFLOAT3 dir = XMFLOAT3(nx[rand_dir], 0, nz[rand_dir]);
				XMFLOAT3 newPos = Vector3::Add(Pos, Vector3::ScalarProduct(dir, speed * fTimeElapsed, false));

				if (ObstacleGrid[1].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
					continue;
				


				for (const auto& monster : monsters[room_num]) {
					if (monster->alive.load() == false || monster->m_id == m_id) continue;
					if (monster->BB.Intersects(BoundingBox(newPos, BB.Extents))) {
						continue;
					}
				}


				Pos = newPos;
				BB.Center = Pos;
				break;
			}
			wander_timer += fTimeElapsed;
			if (wander_timer > 2.f) {
				wander_timer = 0.f;
				wander = false;
				SetState(NPC_State::Idle);
			}
		}
		else {
			const auto& targetPlayer = &clients[room_num][target_id];
			XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
			g_distance = Vector3::XZLength(distanceVector);

			if (clients[room_num][target_id]._state.load() != ST_INGAME)
			{
				SetState(NPC_State::Idle);
				target_id = -1;
				break;
			}
			if (attack_range >= g_distance)
			{
				SetState(NPC_State::Attack);
				break;
			}
			const int collide_range = static_cast<int>(Pos.z / AREA_SIZE);
			XMFLOAT3 vel = Vector3::XZNormalize(distanceVector);
			XMFLOAT3 newPos = Vector3::Add(Pos, Vector3::ScalarProduct(vel, speed * fTimeElapsed, false));

			bool collide = false;

			if (ObstacleGrid[1].at(int(newPos.x) / CELL_SIZE).at(int(newPos.z) / CELL_SIZE) == false)
				collide = true;

			if (collide == false) {
				for (const auto& monster : monsters[room_num]) {
					if (monster->alive.load() == false || monster->m_id == m_id) continue;
					if (monster->BB.Intersects(BoundingBox(newPos, BB.Extents))) {
						collide = true;
						break;
					}
				}
			}

			if (collide == true) {
				Pos = Find_Direction(fTimeElapsed, Pos, targetPlayer->GetPosition());
				BB.Center = Pos;
			}
			else {
				Pos = newPos;
				BB.Center = Pos;
			}
		}
	}
						 break;
	case NPC_State::Attack: {
		const auto& targetPlayer = &clients[room_num][target_id];
		XMFLOAT3 distanceVector = Vector3::Subtract(targetPlayer->GetPosition(), Pos);
		g_distance = Vector3::Length(distanceVector);

		attack_timer -= fTimeElapsed;
		if (targetPlayer->_state != ST_INGAME) {
			SetState(NPC_State::Idle);
			target_id = -1;
			SetAttackTimer(attack_cycle);
			break;
		}
		if (attacked == false && GetAttackTimer() <= attack_cycle / 2.f) {
			for (auto& client : clients[room_num]) {
				if (client._state.load() != ST_INGAME) continue;
				XMFLOAT3 attack_distanceVector = Vector3::Subtract(client.GetPosition(), Pos);
				float g_attack_distance = Vector3::Length(attack_distanceVector);
				if (attack_range > g_attack_distance) {
					lock_guard <mutex> ll{ client._s_lock };
					client.HP -= GetPower();
					if (client.HP <= 0) {
						client._state.store(ST_DEAD);
						for (auto& cl : clients[room_num]) {
							if (cl._state.load() == ST_INGAME || cl._state.load() == ST_DEAD)
								cl.send_update_packet(targetPlayer);
						}
					}
				}
			}
			attacked = true;
			break;
		}
		if (GetAttackTimer() <= 0) {
			if (attack_range <= g_distance)
			{
				SetState(NPC_State::Chase);
				SetAttackTimer(attack_cycle);
			}
			SetAttackTimer(attack_cycle);
			attacked = false;
		}
	}
						  break;
	case NPC_State::Dead: {
		dead_timer -= fTimeElapsed;
		if (dead_timer <= 0) {
			alive.store(false);
		}
	}
						break;
	default:
		break;
	}
}

void InitializeMonsters()
{
	for (int i = 0; i < MAX_ROOM; ++i)
	{
		for (int j = 0; j < monsters[i].size(); ++j)
		{
			if (StagesInfo[j].type == 2)
				monsters[i][j] = new SorcererMonster();
			else if (StagesInfo[j].type == 3)
				monsters[i][j] = new BossMonster();
			else
				monsters[i][j] = new Monster();			
			monsters[i][j]->Initialize(i, StagesInfo[j].id, StagesInfo[j].type, StagesInfo[j].Pos);
		}
	}
}

void FinalizeMonsters()
{
	for (int i = 0; i < monsters.size(); ++i)
	{
		for (int j = 0; j < monsters[i].size(); ++j)
			delete monsters[i][j];
	}
}


void InitializeGrid()
{
	std::ifstream file("map.txt");

	if (file.is_open())
	{
		for (int floor = 0; floor < 2; ++floor)
		{
			for (int x = 0; x < GRID_SIZE_X; ++x)
			{
				for (int z = 0; z < GRID_SIZE_Z; ++z)
				{
					file >> ObstacleGrid[floor][x][z];
				}
			}
		}
		file.close();
	}
	else
	{
		std::cout << "Failed to open file for reading." << endl;
	}
}

void InitializeMap()
{
	int* m_nObjects = new int(0);
	MapObject** m_ppObjects = LoadGameObjectsFromFile("Models/Scene.bin", m_nObjects);

	for (int i = 0; i < *m_nObjects; i++) {
		if (0 == strcmp(m_ppObjects[i]->m_pstrName, "Dense_Floor_mesh") ||
			0 == strcmp(m_ppObjects[i]->m_pstrName, "Ceiling_concrete_base_mesh") ||
			0 == strcmp(m_ppObjects[i]->m_pstrName, "Bedroom_wall_b_06_mesh")) continue;

		if (0 == strcmp(m_ppObjects[i]->m_pstrName, "Key_mesh"))
		{
			Key_Items[0].emplace_back(m_ppObjects[i]->m_xmOOBB, 1, i);
			continue;
		}
		if (0 == strcmp(m_ppObjects[i]->m_pstrName, "2ndRoomCoin")) {
			Key_Items[1].emplace_back(m_ppObjects[i]->m_xmOOBB, 0.17f, i);
			continue;
		}


		int collide_range_min = ((int)m_ppObjects[i]->m_xmOOBB.Center.z - (int)m_ppObjects[i]->m_xmOOBB.Extents.z) / AREA_SIZE;
		int collide_range_max = ((int)m_ppObjects[i]->m_xmOOBB.Center.z + (int)m_ppObjects[i]->m_xmOOBB.Extents.z) / AREA_SIZE;

		for (int j = collide_range_min; j <= collide_range_max; j++) {
			Obstacles[j].emplace_back(m_ppObjects[i]);
		}
	}

	delete m_nObjects;
	delete[] m_ppObjects;
}

void InitializeMonsterInfo()
{
	int ID_constructor = 0;
	random_device rd;
	mt19937 gen(rd());
	uniform_int_distribution<int> x_dis(130, 550);
	uniform_int_distribution<int> z_dis(1300, 2500);
	int t = -1;

	{   // 1stage
		while (ID_constructor < 10) {
			float _x = static_cast<float>(x_dis(gen));
			float _z = static_cast<float>(z_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -63, _z), MONSTER_SIZE);
			bool col = false;
			for (auto& obj : Obstacles[static_cast<int>(_z) / AREA_SIZE])
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}
			if (col) continue;

			if (4 > ID_constructor % 10)
				t = 0;
			else if (7 > ID_constructor % 10)
				t = 1;
			else if (10 > ID_constructor % 10)
				t = 2;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -63, _z), t, ID_constructor);//pat
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}
	{   // 2stage
		gen.seed(rd());
		z_dis.param(uniform_int_distribution<int>::param_type(2600, 3500));
		while (ID_constructor < 20) {
			float _x = static_cast<float>(x_dis(gen));
			float _z = static_cast<float>(z_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -63, _z), MONSTER_SIZE);
			bool col = false;

			for (auto& obj : Obstacles[static_cast<int>(_z) / AREA_SIZE]) {
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}

			if (col) continue;

			if (4 > ID_constructor % 10)
				t = 0;
			else if (7 > ID_constructor % 10)
				t = 1;
			else if (10 > ID_constructor % 10)
				t = 2;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -63, _z), t, ID_constructor);
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}
	{   // 3stage
		gen.seed(rd());
		x_dis.param(uniform_int_distribution<int>::param_type(130, 320));
		z_dis.param(uniform_int_distribution<int>::param_type(3700, 4400));
		while (ID_constructor < 30) {
			float _x = static_cast<float>(x_dis(gen));
			float _z = static_cast<float>(z_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -304, _z), MONSTER_SIZE);
			bool col = false;
			for (auto& obj : Obstacles[static_cast<int>(_z) / AREA_SIZE])
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}
			if (col) continue;

			if (4 > ID_constructor % 10)
				t = 0;
			else if (7 > ID_constructor % 10)
				t = 1;
			else if (10 > ID_constructor % 10)
				t = 2;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -304, _z), t, ID_constructor);
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}
	{   // 4stage
		gen.seed(rd());
		x_dis.param(uniform_int_distribution<int>::param_type(130, 550));
		z_dis.param(uniform_int_distribution<int>::param_type(2600, 3500));
		while (ID_constructor < 40) {
			float _x = static_cast<float>(x_dis(gen));
			float _z = static_cast<float>(z_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -304, _z), MONSTER_SIZE);
			bool col = false;
			for (auto& obj : Obstacles[static_cast<int>(_z) / AREA_SIZE])
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}
			if (col) continue;

			if (4 > ID_constructor % 10)
				t = 0;
			else if (7 > ID_constructor % 10)
				t = 1;
			else if (10 > ID_constructor % 10)
				t = 2;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -304, _z), t, ID_constructor);
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}
	{   // 5stage
		gen.seed(rd());
		z_dis.param(uniform_int_distribution<int>::param_type(1300, 2450));
		while (ID_constructor < 50) {
			float _x = static_cast<float>(x_dis(gen));
			float _z = static_cast<float>(z_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -304, _z), MONSTER_SIZE);
			bool col = false;
			for (auto& obj : Obstacles[static_cast<int>(_z) / AREA_SIZE])
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}
			if (col) continue;

			if (4 > ID_constructor % 10)
				t = 0;
			else if (7 > ID_constructor % 10)
				t = 1;
			else if (10 > ID_constructor % 10)
				t = 2;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -304, _z), t, ID_constructor);
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}

	{   // 6stage
		gen.seed(rd());
		z_dis.param(uniform_int_distribution<int>::param_type(300, 1100));
		while (ID_constructor < 59) {
			float _x = static_cast<float>(x_dis(gen));
			float _z = static_cast<float>(z_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -304, _z), MONSTER_SIZE);
			bool col = false;
			for (auto& obj : Obstacles[static_cast<int>(_z) / AREA_SIZE])
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}
			if (col) continue;

			if (4 > ID_constructor % 10)
				t = 0;
			else if (7 > ID_constructor % 10)
				t = 1;
			else if (10 > ID_constructor % 10)
				t = 2;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -304, _z), t, ID_constructor);
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}

	{	// 6stage
		gen.seed(rd());
		while (ID_constructor < 60) {
			float _x = static_cast<float>(x_dis(gen));
			BoundingBox test = BoundingBox(XMFLOAT3(_x, -304, 300), BOSS_MONSTER_SIZE);
			bool col = false;
			for (auto& obj : Obstacles[static_cast<int>(300) / AREA_SIZE])
				if (obj->m_xmOOBB.Intersects(test)) {
					col = true;
					break;
				}
			for (int i = ID_constructor - 1; i >= ID_constructor - ID_constructor % 10; i--) {
				if (BoundingBox(StagesInfo[i].Pos, MONSTER_SIZE).Intersects(test)) {
					col = true;
					break;
				}
			}
			if (col) continue;
			MonsterInfo MI = MonsterInfo(XMFLOAT3(_x, -304, 300), 3, ID_constructor);
			StagesInfo.push_back(MI);
			//cout << ID_constructor << " - " << MI.type << endl;
			//Vector3::Print(MI.Pos);
			ID_constructor++;
		}
	}
}