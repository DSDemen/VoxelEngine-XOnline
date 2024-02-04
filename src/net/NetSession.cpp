#include "NetSession.h"

#include <iostream>
#include <stdlib.h>
#include <ctime>

#include "../world/Level.h"
#include "../world/World.h"
#include "../objects/Player.h"
#include "../voxels/Chunks.h"
#include "../content/Content.h"
#include "../voxels/Block.h"
#include "../engine.h"
#include "../voxels/Chunk.h"
#include "../voxels/Chunks.h"
#include "../voxels/ChunksStorage.h"
#include "../lighting/Lighting.h"
#include "../frontend/WorldRenderer.h"
#include "../logic/PlayerController.h"
#include "../logic/BlocksController.h"

namespace fs = std::filesystem;

NetSession *NetSession::sessionInstance = nullptr;
ConnectionData *NetSession::connData = nullptr;

void NetSession::TerminateSession()
{
    if(sessionInstance == nullptr)
        return;
    delete sessionInstance;
    sessionInstance = nullptr;
}

NetSession *NetSession::GetSessionInstance()
{
    return sessionInstance;
}

NetSession::NetSession(NetMode mode) : netMode(mode), socket(Socket())
{
    messagesBuffer = std::vector<NetMessage>();
    serverUpdate = false;
}

NetSession::~NetSession()
{
    std::cout << "[INFO]: Terminating session" << std::endl;
    for(auto pair : users)
    {
        socket.CleanUpSocket(pair.first);
        free(pair.second);
    }
    users.clear();
    socket.CloseSocket();
}

bool NetSession::StartSession(Engine *eng, const int port)
{
    std::cout << "[INFO]: Server started on port:" << port << std::endl;
    if(sessionInstance)
    {
        return false;
    }

    sessionInstance = new NetSession(NetMode::PLAY_SERVER);
    sessionInstance->engine = eng;
    if(sessionInstance->socket.StartupServer(port))
    {
        std::cout << "[INFO]: Creating NetSession. NetMode = " << sessionInstance->netMode << std::endl;
        sessionInstance->addUser(NetUserRole::AUTHORITY, 0);
        std::cout << "[INFO]: Server started" << std::endl;
        return true;
    }
    TerminateSession();
    return false;
}

bool NetSession::ConnectToSession(const char *ip, const int port, Engine *eng, bool versionChecking, bool contentChecking)
{
    if(sessionInstance)
    {
        return false;
    }
    
    sessionInstance = new NetSession(NetMode::CLIENT);
    std::cout << "Connecting to " << ip << std::endl;

    sessionInstance->engine = eng;

    if(!sessionInstance->socket.ConnectTo(ip, port))
    {
        std::cout << "[ERROR]: Couldn't connect. Error code: " << errno << std::endl;
        TerminateSession();
        return false;
    }
    std::cout << "[INFO]: Connected, waiting for initial message" << std::endl;
    std::vector<char> msg;
    if(sessionInstance->socket.RecieveMessage(msg, MaxNetSize(), sessionInstance->socket.sockfd, true) > 0)
    {
        std::cout << "[INFO]: Connection message: " << msg.data() << std::endl;

        std::unique_ptr<dynamic::Map> data = json::parse(msg.data());
        connData = new ConnectionData();

        data->num("seed", connData->seed);
        data->num("count", connData->blockCount);
        data->map("version")->num("major", connData->major);
        data->map("version")->num("minor", connData->minor);
        data->num("user", connData->userID );
        connData->name = data->getStr("name", "err");
        
        size_t size = data->list("content")->size();
        for(size_t i = 0; i < size; i++)
        {
            auto val = data->list("content")->values[i].get();
            if(val->type == dynamic::valtype::string)
            {
                connData->contentNames.push_back(val->value.str->c_str());
            }
        }

        if(versionChecking)
        {
            if(connData->major != ENGINE_VERSION_MAJOR || connData->minor != ENGINE_VERSION_MINOR)
            {
                TerminateSession();
                return false;
            }
        }

        if(contentChecking)
        {
            if(connData->blockCount != eng->getContent()->getIndices()->countBlockDefs())
            {
                TerminateSession();
                return false;
            }

            // check if player have the same content packs

            for(std::string name : connData->contentNames)
            {
                if(std::find_if(eng->getContentPacks().begin(), 
                            eng->getContentPacks().end(), 
                            [name] (const ContentPack &p) { return p.title == name; }) == eng->getContentPacks().end())
                            {
                                return false;
                            }
            }
        }
        sessionInstance->addUser(NetUserRole::LOCAL, connData->userID);
        return true;
    }
    std::cout << "[ERROR]: Couldn't get connection message. Error code: " << errno << std::endl;
    TerminateSession();
    return false;
}
void NetSession::handleConnection(socketfd ui)
{
    addUser(NetUserRole::REMOTE, ui);
    std::cout << "[INFO]: Handling new connection, uniq_id = " << ui << std::endl;

    sharedLevel->world->write(sharedLevel);

    size_t blCount = sharedLevel->content->getIndices()->countBlockDefs();

    dynamic::Map worldData = dynamic::Map();
    worldData.put("seed", sharedLevel->world->getSeed());
    worldData.put("count", blCount);
    worldData.put("name", sharedLevel->world->getName());
    worldData.put("user", ui);

    dynamic::Map& verObj = worldData.putMap("version");
    verObj.put("major", ENGINE_VERSION_MAJOR);
    verObj.put("minor", ENGINE_VERSION_MINOR);

    dynamic::List& cont = worldData.putList("content");

    size_t size = engine->getContentPacks().size();
    for(size_t i = 0; i < size; i++)
    {
        cont.put(engine->getContentPacks()[i].title);
    }

    std::string msg = json::stringify(&worldData, false, "");
    socket.SendMessage(msg.c_str(), msg.length(), ui, false);
}

NetMode NetSession::GetSessionType()
{
    if(!sessionInstance)
    {
        return NetMode::STAND_ALONE;
    }
    return sessionInstance->netMode;
}


void NetSession::SetSharedLevel(Level *sl) noexcept
{
    if(sessionInstance)
    {
        sessionInstance->sharedLevel = sl;
        std::cout << "shared level been set\n";
    }
}

NetUser *NetSession::addUser(NetUserRole role, uniqueUserID id)
{
    NetUser *user = new NetUser(role, nullptr, id);
    user->isConnected = true;
    std::cout << "[INFO]: Adding NetUser with userID  = "  << user->GetUniqueUserID() << std::endl;
    users.insert_or_assign(id, user);
    return user;
}

void NetSession::packMessages(NetPackage *dst)
{
    size_t messages = std::min(MAX_MESSAGES_PER_PACKET - dst->GetMessagesCount(), messagesBuffer.size());

    while (messages) {
        messages--;
        dst->AddMessage(messagesBuffer.back());
        messagesBuffer.pop_back();
    }
}

void NetSession::processPackage(NetPackage *pkg)
{
    for(int i = 0; i < (int)pkg->GetMessagesCount(); i++)
    {
        NetMessage msg = pkg->GetMessage(i);
        switch(msg.action)
        {
            case UNKOWN:
            break;
            case NetAction::SERVER_UPDATE:
                serverUpdate = true;
                sharedLevel->getWorld()->daytime = msg.coordinates.x;
                WorldRenderer::fog = msg.coordinates.y;
            break;
            case NetAction::MODIFY:
                sharedLevel->chunks->set((int)msg.coordinates.x, (int)msg.coordinates.y, 
                                        (int)msg.coordinates.z, msg.block, msg.states);
                if(netMode == NetMode::CLIENT) // a bit better
                {
                    if(!messagesBuffer.empty())
                        messagesBuffer.pop_back(); // but still some shit
                }
            break;
            case NetAction::FETCH:
                if(netMode == NetMode::CLIENT)
                {
                    auto chunk = sharedLevel->chunksStorage->get((int)msg.coordinates.x, (int)msg.coordinates.z);
                    if(chunk == nullptr) continue;
                    if(chunk->isLoaded()) continue;
                    if(msg.data)
                    {
                        chunk->decode(msg.data);
                        for (size_t i = 0; i < CHUNK_VOL; i++) {
                            blockid_t id = chunk->voxels[i].id;
                            if (sharedLevel->content->getIndices()->getBlockDef(id) == nullptr) {
                                std::cout << "corruped block detected at " << i << " of chunk ";
                                std::cout << chunk->x << "x" << chunk->z;
                                std::cout << " -> " << (int)id << std::endl;
                                chunk->voxels[i].id = 11;
                            }
                        }

                        chunk->updateHeights();
                        Lighting::prebuildSkyLight(chunk.get(), sharedLevel->content->getIndices());
                        chunk->setLoaded(true);
                        chunk->setReady(true);
                        chunk->setLoadedLights(false);
                        chunk->setLighted(false);
                    }                            
                }
                else if(netMode == NetMode::PLAY_SERVER)
                {
                    ubyte *chunkData = serverGetChunk((int)msg.coordinates.x, (int)msg.coordinates.z);

                    if(chunkData)
                    {
                        NetMessage fm = NetMessage();
                        fm.action = NetAction::FETCH;
                        fm.coordinates.x = msg.coordinates.x;
                        fm.coordinates.z = msg.coordinates.z;
                        fm.data = chunkData;
                        RegisterMessage(fm);
                    }
                }
            break;
        }
    }
}

void NetSession::serverRoutine()
{
    if(socket.UpdateServer(users))
    {
        while(socket.HasNewConnection())
        {
            socketfd cl = socket.GetNewConnection();
            handleConnection(cl);
        }

        NetPackage inPkg = NetPackage();
        while(socket.RecievePackage(&inPkg))
        {
            processPackage(&inPkg);
        }

        NetPackage servPkg = NetPackage();
        NetMessage upd = NetMessage();
        upd.action = NetAction::SERVER_UPDATE;
        if(sharedLevel)
        {
            upd.coordinates.x = sharedLevel->getWorld()->daytime;
            upd.coordinates.y = WorldRenderer::fog;
        }
        servPkg.AddMessage(upd);

        packMessages(&servPkg);
         
        for(auto pair : users)
        {
            if(pair.second->isConnected && pair.first != 0)
            {
                socket.SendPackage(&servPkg, pair.first);
            }
        }
    }
}

void NetSession::clientRoutine()
{
    if(socket.UpdateClient())
    {
        NetPackage inPkg = NetPackage();
        while(socket.RecievePackage(&inPkg))
        {
            processPackage(&inPkg);
        }
    }

    if(serverUpdate)
    {
        auto it = chunksQueue.begin();
        if(it != chunksQueue.end())
        {
            NetMessage fm = NetMessage();
            fm.action = NetAction::FETCH;
            fm.coordinates.x = it->second->x;
            fm.coordinates.z = it->second->z;
            RegisterMessage(fm);
            chunksQueue.erase(it->first);
        }

        NetPackage pkgToSend = NetPackage();

        packMessages(&pkgToSend);

        if(pkgToSend.GetMessagesCount() > 0)
        {
            socket.SendPackage(&pkgToSend, socket.sockfd);
        }
        serverUpdate = false;
    }
}

void NetSession::ClientFetchChunk(std::shared_ptr<Chunk> chunk, int x, int z)
{
    if(chunksQueue.find({x, z}) == chunksQueue.end())
    {
        chunksQueue.insert_or_assign({x, z}, chunk);
    }
}

ubyte *NetSession::serverGetChunk(int x, int z) const
{
    std::shared_ptr<Chunk> chunk = sharedLevel->chunksStorage->get(x, z);

    // get chunk from memory
    if(chunk)
    {
        if(chunk->isLoaded())
        {
            return chunk->encode();
        }
    }

    // load and get chunk from hard drive
    chunk = sharedLevel->chunksStorage->create(x, z);    
    if(chunk->isLoaded())
    {
        return chunk->encode();
    }

    // generate chunk
    
    return nullptr;
}

float serv_delta = 0;
void NetSession::Update(float delta) noexcept
{
    if(!sharedLevel) return;

    serv_delta += delta;
    switch(netMode)
    {
        case NetMode::PLAY_SERVER:
            if(serv_delta >= 1 / SERVER_BIT_RATE)
            {
                serverRoutine();
                serv_delta = 0;
            }
        break;
        case NetMode::CLIENT:
            clientRoutine();
        break;
        case NetMode::STAND_ALONE:
        return;
    }
}

void NetSession::RegisterMessage(const NetMessage msg) noexcept
{
    messagesBuffer.push_back(msg);
}

NetUser *NetSession::GetUser(size_t i)
{
    assert(0 && "DO NOT FUCKING USE IT NOW, YOU RISKY KIDDO!");
    if(sessionInstance)
    {
        return sessionInstance->users[i];
    }
    return nullptr;
}


