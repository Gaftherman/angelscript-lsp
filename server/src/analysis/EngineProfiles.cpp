#include "analysis/EngineProfiles.h"
#include <algorithm>
#include <cctype>

namespace angel_lsp::analysis
{
    namespace
    {
        std::string ToLowerString(std::string_view str)
        {
            std::string result;
            result.reserve(str.size());
            for (char c : str)
            {
                result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return result;
        }

        constexpr std::string_view STANDARD_PROFILE_STUB = R"angelscript(
// A partially declared type is worse than an undeclared one: this analyzer stays silent about a
// name it cannot see at all, but a class it *can* see is one it will report missing members on. The
// declarations below therefore track the add-ons' own registrations rather than being a convenient
// subset - `datetime` previously declared `getYear()` where the add-on registers the property
// accessor `get_year()`, and every correct script using it drew "Class 'datetime' has no member".
//
// See tests/fixtures/full-addons.as.predefined for the full surface and its provenance.

class string
{
    string();
    string(const string &in other);
    uint length() const;
    void resize(uint size);
    bool isEmpty() const;
    uint8& opIndex(uint index);
    const uint8& opIndex(uint index) const;
    string substr(uint start = 0, int count = -1) const;
    int findFirst(const string &in sub, uint start = 0) const;
    int findFirstOf(const string &in chars, uint start = 0) const;
    int findFirstNotOf(const string &in chars, uint start = 0) const;
    int findLast(const string &in sub, int start = -1) const;
    int findLastOf(const string &in chars, int start = -1) const;
    int findLastNotOf(const string &in chars, int start = -1) const;
    void insert(uint pos, const string &in other);
    void erase(uint pos, int count = -1);
    array<string>@ split(const string &in delimiter) const;
    int opCmp(const string &in other) const;
    bool opEquals(const string &in other) const;
    string opAdd(const string &in other) const;
    string& opAssign(const string &in other);
    string& opAddAssign(const string &in other);
    string& opAssign(double);
    string& opAddAssign(double);
    string opAdd(double) const;
    string opAdd_r(double) const;
    string& opAssign(float);
    string& opAddAssign(float);
    string opAdd(float) const;
    string opAdd_r(float) const;
    string& opAssign(int64);
    string& opAddAssign(int64);
    string opAdd(int64) const;
    string opAdd_r(int64) const;
    string& opAssign(uint64);
    string& opAddAssign(uint64);
    string opAdd(uint64) const;
    string opAdd_r(uint64) const;
    string& opAssign(bool);
    string& opAddAssign(bool);
    string opAdd(bool) const;
    string opAdd_r(bool) const;
}

/// The list factory the array add-on registers:
///   asBEHAVE_LIST_FACTORY, "array<T>@ f(int&in type, int&in list) {repeat T}"
/// @listpattern {repeat T}
class array<T>
{
    array();
    array(uint initialSize);
    array(uint initialSize, const T &in value);
    uint length() const;
    void resize(uint size);
    void reserve(uint capacity);
    bool isEmpty() const;
    void insertAt(uint index, const T &in value);
    void insertAt(uint index, const array<T> &inout arr);
    void removeAt(uint index);
    void removeRange(uint start, uint count);
    void insertLast(const T &in value);
    void removeLast();
    void sortAsc();
    void sortAsc(uint startAt, uint count);
    void sortDesc();
    void sortDesc(uint startAt, uint count);
    void reverse();
    int find(const T &in value) const;
    int find(uint startAt, const T &in value) const;
    int findByRef(const T &in value) const;
    int findByRef(uint startAt, const T &in value) const;
    bool opEquals(const array<T> &in other) const;
    T& opIndex(uint index);
    const T& opIndex(uint index) const;
    uint opForBegin() const;
    bool opForEnd(uint index) const;
    uint opForNext(uint index) const;
    const T& opForValue0(uint index) const;
    uint opForValue1(uint index) const;
}

/// The list factory the dictionary add-on registers:
///   asBEHAVE_LIST_FACTORY, "dictionary @f(int &in) {repeat {string, ?}}"
/// @listpattern {repeat {string, ?}}
class dictionary
{
    dictionary();
    void set(const string &in key, const ? &in value);
    bool get(const string &in key, ? &out value) const;
    void set(const string &in key, const int64 &in value);
    bool get(const string &in key, int64 &out value) const;
    void set(const string &in key, const double &in value);
    bool get(const string &in key, double &out value) const;
    bool exists(const string &in key) const;
    bool delete(const string &in key);
    void deleteAll();
    bool isEmpty() const;
    uint getSize() const;
    array<string>@ getKeys() const;
    dictionaryValue& opIndex(const string &in key);
    const dictionaryValue& opIndex(const string &in key) const;
    dictionaryIter@ opForBegin() const;
    bool opForEnd(dictionaryIter@ it) const;
    dictionaryIter@ opForNext(dictionaryIter@ it) const;
    const dictionaryValue& opForValue0(dictionaryIter@ it) const;
    const string& opForValue1(dictionaryIter@ it) const;
}

class dictionaryIter
{
}

class dictionaryValue
{
    dictionaryValue();
    dictionaryValue& opAssign(const dictionaryValue &in other);
    dictionaryValue& opAssign(const ? &in value);
    dictionaryValue& opHndlAssign(const ? &in value);
    void opCast(? &out value);
    void opConv(? &out value);
}

/// `ref` assigns through opHndlAssign - it declares no opAssign, so `ref r = @obj;` is rejected by
/// the real compiler and `ref r(@obj);` is the way in.
class ref
{
    ref();
    ref(const ref &in other);
    ref(const ? &in value);
    void opCast(? &out value);
    ref& opHndlAssign(const ref &in other);
    ref& opHndlAssign(const ? &in value);
    bool opEquals(const ref &in other) const;
    bool opEquals(const ? &in value) const;
}

/// The datetime add-on exposes its fields as property accessors, spelled `get_year` and so on.
class datetime
{
    datetime();
    datetime(const datetime &in other);
    datetime(uint year, uint month, uint day, uint hour = 0, uint minute = 0, uint second = 0);
    datetime& opAssign(const datetime &in other);
    uint get_year() const;
    uint get_month() const;
    uint get_day() const;
    uint get_hour() const;
    uint get_minute() const;
    uint get_second() const;
    uint get_weekDay() const;
    bool setDate(uint year, uint month, uint day);
    bool setTime(uint hour, uint minute, uint second);
    int64 opSub(const datetime &in other) const;
    datetime opAdd(int64 seconds) const;
    datetime opAdd_r(int64 seconds) const;
    datetime& opAddAssign(int64 seconds);
    datetime opSub(int64 seconds) const;
    datetime& opSubAssign(int64 seconds);
    bool opEquals(const datetime &in other) const;
    int opCmp(const datetime &in other) const;
}

// String utilities - add_on/scriptstdstring/scriptstdstring_utils.cpp
string formatInt(int64 val, const string &in options = "", uint width = 0);
string formatUInt(uint64 val, const string &in options = "", uint width = 0);
string formatFloat(double val, const string &in options = "", uint width = 0, uint precision = 0);
int64 parseInt(const string &in str, uint base = 10, uint &out byteCount = 0);
uint64 parseUInt(const string &in str, uint base = 10, uint &out byteCount = 0);
double parseFloat(const string &in str, uint &out byteCount = 0);
string join(const array<string> &in arr, const string &in delimiter);

float sin(float rad);
float cos(float rad);
float tan(float rad);
float asin(float val);
float acos(float val);
float atan(float val);
float atan2(float y, float x);
float sqrt(float val);
float pow(float base, float exp);
float abs(float val);
float floor(float val);
float ceil(float val);
float round(float val);
float min(float a, float b);
float max(float a, float b);
float clamp(float val, float minVal, float maxVal);
void print(const string &in msg);
void println(const string &in msg);
)angelscript";

        constexpr std::string_view SVENCOOP_PROFILE_STUB = R"angelscript(
class Vector
{
    float x;
    float y;
    float z;
    Vector();
    Vector(float x, float y, float z);
    float Length() const;
    float Length2D() const;
    Vector Normalize() const;
    Vector opAdd(const Vector &in other) const;
    Vector opSub(const Vector &in other) const;
    Vector opMul(float scalar) const;
    Vector opDiv(float scalar) const;
    bool opEquals(const Vector &in other) const;
}

class Vector2D
{
    float x;
    float y;
    Vector2D();
    Vector2D(float x, float y);
    float Length() const;
}

class RGBA
{
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 a;
    RGBA();
    RGBA(uint8 r, uint8 g, uint8 b, uint8 a = 255);
}

class TraceResult
{
    bool fAllSolid;
    bool fStartSolid;
    bool fInOpen;
    bool fInWater;
    float flFraction;
    Vector vecEndPos;
    float flPlaneDist;
    Vector vecPlaneNormal;
    CBaseEntity@ pHit;
    int iHitgroup;
}

class CBaseEntity
{
    Vector pev_origin;
    Vector pev_angles;
    Vector pev_velocity;
    int pev_health;
    int pev_max_health;
    bool IsAlive() const;
    bool IsPlayer() const;
    void TakeDamage(CBaseEntity@ pInflictor, CBaseEntity@ pAttacker, float flDamage, int bitsDamageType);
    void Killed(CBaseEntity@ pAttacker, int iGib);
    Vector GetOrigin() const;
    void SetOrigin(const Vector &in vecOrigin);
}

class CBasePlayer : CBaseEntity
{
    bool IsConnected() const;
    string GetPlayerName() const;
    void GiveNamedItem(const string &in itemName);
    void SetArmor(int armor);
    int GetArmor() const;
}

class CBasePlayerWeapon : CBaseEntity
{
    int m_iClip;
    int m_iDefaultAmmo;
    bool PlayEmptySound();
}

class EngineFuncs
{
    void ServerPrint(const string &in msg);
    void ClientPrintf(CBasePlayer@ pPlayer, int printType, const string &in msg);
    void DropToFloor(CBaseEntity@ pEntity);
    void MakeVectors(const Vector &in angles);
}

class PlayerFuncs
{
    CBasePlayer@ FindPlayerByIndex(int index);
    void SayText(CBasePlayer@ pPlayer, const string &in msg);
    void SayTextAll(CBasePlayer@ pSpeaker, const string &in msg);
    void RespawnPlayer(CBasePlayer@ pPlayer, bool bGib = false);
}

class EntityFuncs
{
    CBaseEntity@ CreateEntity(const string &in className, const dictionary &in keyvalues = dictionary(), bool spawn = true);
    void Remove(CBaseEntity@ pEntity);
    TraceResult TraceLine(const Vector &in vecStart, const Vector &in vecEnd, int ignoreMonsters, CBaseEntity@ pIgnoreEntity);
}

class SoundSystem
{
    void PlaySound(CBaseEntity@ pEntity, int channel, const string &in sample, float volume, float attenuation, int flags = 0, int pitch = 100);
    void EmitSound(CBaseEntity@ pEntity, int channel, const string &in sample, float volume, float attenuation);
}

class Scheduler
{
    void SetTimeout(const string &in funcName, float delay);
    void SetInterval(const string &in funcName, float interval);
}

EngineFuncs g_EngineFuncs;
PlayerFuncs g_PlayerFuncs;
EntityFuncs g_EntityFuncs;
SoundSystem g_SoundSystem;
Scheduler g_Scheduler;
)angelscript";

        constexpr std::string_view URHO3D_PROFILE_STUB = R"angelscript(
class Vector2
{
    float x;
    float y;
    Vector2();
    Vector2(float x, float y);
    float Length() const;
}

class Vector3
{
    float x;
    float y;
    float z;
    Vector3();
    Vector3(float x, float y, float z);
    float Length() const;
    Vector3 Normalized() const;
    Vector3 CrossProduct(const Vector3 &in rhs) const;
    float DotProduct(const Vector3 &in rhs) const;
}

class Vector4
{
    float x;
    float y;
    float z;
    float w;
    Vector4();
    Vector4(float x, float y, float z, float w);
}

class Quaternion
{
    float w;
    float x;
    float y;
    float z;
    Quaternion();
    Quaternion(float angle, const Vector3 &in axis);
}

class Matrix3
{
    Matrix3();
}

class Matrix4
{
    Matrix4();
}

class Color
{
    float r;
    float g;
    float b;
    float a;
    Color();
    Color(float r, float g, float b, float a = 1.0f);
}

class StringHash
{
    StringHash();
    StringHash(const string &in str);
    uint value;
}

class Variant
{
    Variant();
    Variant(int val);
    Variant(float val);
    Variant(const string &in val);
    Variant(const Vector3 &in val);
}

class VariantMap
{
    VariantMap();
    Variant& opIndex(const StringHash &in key);
}

class RefCounted
{
}

class Object : RefCounted
{
    StringHash GetType() const;
    string GetTypeName() const;
    void SubscribeToEvent(const StringHash &in eventType, const string &in handlerMethodName);
    void SendEvent(const StringHash &in eventType, const VariantMap &in eventData = VariantMap());
    void UnsubscribeFromEvent(const StringHash &in eventType);
}

class Serializable : Object
{
}

class Node : Serializable
{
    Node();
    Node@ CreateChild(const string &in name = "");
    Component@ CreateComponent(const string &in typeName);
    Component@ GetComponent(const string &in typeName) const;
    void SetPosition(const Vector3 &in pos);
    Vector3 GetPosition() const;
    void SetRotation(const Quaternion &in rot);
    Quaternion GetRotation() const;
    void SetScale(const Vector3 &in scale);
    Vector3 GetScale() const;
    void Remove();
}

class Component : Serializable
{
    Node@ GetNode() const;
    bool IsEnabled() const;
    void SetEnabled(bool enable);
}

class Scene : Node
{
    Scene();
    bool LoadXML(const string &in fileName);
    bool SaveXML(const string &in fileName) const;
    void Clear();
}

class UIElement : Object
{
}

class Resource : Object
{
    string GetName() const;
}

class RigidBody : Component
{
    void SetMass(float mass);
    float GetMass() const;
    void ApplyForce(const Vector3 &in force);
}

class Camera : Component
{
    void SetFov(float fov);
    float GetFov() const;
}

class Light : Component
{
    void SetColor(const Color &in color);
    void SetRange(float range);
}

void SubscribeToEvent(const StringHash &in eventType, const string &in handlerMethodName);
void SendEvent(const StringHash &in eventType, const VariantMap &in eventData = VariantMap());
void Print(const string &in msg);
)angelscript";

        constexpr std::string_view OPENXRAY_PROFILE_STUB = R"angelscript(
class Fvector
{
    float x;
    float y;
    float z;
    Fvector();
    Fvector(float x, float y, float z);
    float distance_to(const Fvector &in other) const;
}

class Fvector2
{
    float x;
    float y;
    Fvector2();
}

class Fmatrix
{
    Fmatrix();
}

class Fcolor
{
    float r;
    float g;
    float b;
    float a;
    Fcolor();
}

class ini_file
{
    ini_file(const string &in fname);
    bool section_exist(const string &in section) const;
    bool line_exist(const string &in section, const string &in line) const;
    string r_string(const string &in section, const string &in line) const;
    int r_s32(const string &in section, const string &in line) const;
    float r_float(const string &in section, const string &in line) const;
    bool r_bool(const string &in section, const string &in line) const;
}

class game_object
{
    uint id() const;
    string name() const;
    string section() const;
    int clsid() const;
    Fvector position() const;
    Fvector direction() const;
    float health;
    bool alive() const;
    void kill(game_object@ killer);
    void give_info_portion(const string &in info);
    bool has_info(const string &in info) const;
}

class level
{
    string name() const;
    game_object@ object_by_id(uint id);
    game_object@ main_input_receiver();
}

class alife_simulator
{
    game_object@ object(uint id) const;
    game_object@ actor() const;
    game_object@ create(const string &in section, const Fvector &in pos, uint lvid, uint gvid);
    void release(game_object@ obj, bool b);
}

alife_simulator@ alife();
game_object@ get_actor();
uint time_global();
void log(const string &in msg);
void printf(const string &in format);
)angelscript";

        constexpr std::string_view OOTP_PROFILE_STUB = R"angelscript(
class OOTPPlayer
{
    int id;
    string firstName;
    string lastName;
    int age;
    int position;
    int teamId;
    int overallRating;
    int potentialRating;
}

class OOTPTeam
{
    int id;
    string name;
    string nickname;
    string abbreviation;
    int wins;
    int losses;
    array<OOTPPlayer@>@ GetRoster() const;
}

class OOTPLeague
{
    int id;
    string name;
    array<OOTPTeam@>@ GetTeams() const;
}

class OOTPGame
{
    int homeTeamId;
    int awayTeamId;
    int homeScore;
    int awayScore;
    bool isFinished;
}

class OOTPContext
{
    OOTPLeague@ GetActiveLeague();
    OOTPPlayer@ GetPlayer(int id);
    OOTPTeam@ GetTeam(int id);
}

OOTPContext g_OOTP;
)angelscript";
    }

    EngineProfileKind ParseEngineProfileKind(std::string_view name)
    {
        std::string lower = ToLowerString(name);

        if (lower == "none")
        {
            return EngineProfileKind::None;
        }
        if (lower == "standard" || lower == "std" || lower == "default")
        {
            return EngineProfileKind::Standard;
        }
        if (lower == "svencoop" || lower == "sven" || lower == "sven_coop" || lower == "svenco-op")
        {
            return EngineProfileKind::SvenCoop;
        }
        if (lower == "urho3d" || lower == "urho" || lower == "atomic")
        {
            return EngineProfileKind::Urho3D;
        }
        if (lower == "openxray" || lower == "xray" || lower == "stalker")
        {
            return EngineProfileKind::OpenXRay;
        }
        if (lower == "ootp" || lower == "ootpbaseball")
        {
            return EngineProfileKind::OOTP;
        }
        if (lower == "auto" || lower == "detect")
        {
            return EngineProfileKind::Auto;
        }

        // Unrecognised. Standard is the right thing to LOAD - a workspace with a typo in its
        // profile name is better off with the standard library than with nothing - but the caller
        // has to be able to say so, which is what IsKnownEngineProfileName is for. Returning
        // Standard silently meant a mistyped `svencop` gave you a workspace with no host types and
        // nothing on screen explaining why.
        return EngineProfileKind::Standard;
    }

    bool IsKnownEngineProfileName(std::string_view name)
    {
        const std::string lower = ToLowerString(name);
        static constexpr std::string_view k_known[] = {
            "none",
            "standard", "std", "default",
            "svencoop", "sven", "sven_coop", "svenco-op",
            "urho3d", "urho", "atomic",
            "openxray", "xray", "stalker",
            "ootp", "ootpbaseball",
            "auto", "detect",
        };

        for (const std::string_view candidate : k_known)
        {
            if (lower == candidate)
                return true;
        }
        return false;
    }

    std::string_view EngineProfileKindToString(EngineProfileKind kind)
    {
        switch (kind)
        {
            case EngineProfileKind::None:
                return "none";
            case EngineProfileKind::Standard:
                return "standard";
            case EngineProfileKind::SvenCoop:
                return "svencoop";
            case EngineProfileKind::Urho3D:
                return "urho3d";
            case EngineProfileKind::OpenXRay:
                return "openxray";
            case EngineProfileKind::OOTP:
                return "ootp";
            case EngineProfileKind::Auto:
                return "auto";
        }
        return "standard";
    }

    std::vector<std::string_view> GetAvailableEngineProfiles()
    {
        return {
            "none",
            "standard",
            "svencoop",
            "urho3d",
            "openxray",
            "ootp",
            "auto"
        };
    }

    std::string_view GetProfileStubSource(EngineProfileKind kind)
    {
        switch (kind)
        {
            case EngineProfileKind::None:
                return "";
            case EngineProfileKind::Standard:
                return STANDARD_PROFILE_STUB;
            case EngineProfileKind::SvenCoop:
                return SVENCOOP_PROFILE_STUB;
            case EngineProfileKind::Urho3D:
                return URHO3D_PROFILE_STUB;
            case EngineProfileKind::OpenXRay:
                return OPENXRAY_PROFILE_STUB;
            case EngineProfileKind::OOTP:
                return OOTP_PROFILE_STUB;
            case EngineProfileKind::Auto:
                return STANDARD_PROFILE_STUB;
        }
        return "";
    }

    std::string GetProfileSyntheticUri(EngineProfileKind kind)
    {
        return std::string(k_profileUriPrefix) + std::string(EngineProfileKindToString(kind)) + ".as.predefined";
    }

    EngineProfileKind DetectEngineProfileFromWorkspace(const std::vector<std::string> &fileNamesOrSamples)
    {
        for (const auto &item : fileNamesOrSamples)
        {
            std::string lower = ToLowerString(item);

            if (lower.find("svencoop") != std::string::npos ||
                lower.find("sven") != std::string::npos ||
                lower.find("cbaseplayer") != std::string::npos ||
                lower.find("g_playerfuncs") != std::string::npos ||
                lower.find("g_enginefuncs") != std::string::npos)
            {
                return EngineProfileKind::SvenCoop;
            }

            if (lower.find("urho3d") != std::string::npos ||
                lower.find("urho") != std::string::npos ||
                lower.find("atomic") != std::string::npos ||
                lower.find("subscribetoevent") != std::string::npos ||
                lower.find("variantmap") != std::string::npos)
            {
                return EngineProfileKind::Urho3D;
            }

            if (lower.find("openxray") != std::string::npos ||
                lower.find("xray") != std::string::npos ||
                lower.find("stalker") != std::string::npos ||
                lower.find("ini_file") != std::string::npos ||
                lower.find("alife_simulator") != std::string::npos)
            {
                return EngineProfileKind::OpenXRay;
            }

            if (lower.find("ootp") != std::string::npos ||
                lower.find("ootpplayer") != std::string::npos ||
                lower.find("ootpteam") != std::string::npos)
            {
                return EngineProfileKind::OOTP;
            }
        }

        return EngineProfileKind::Standard;
    }
}
