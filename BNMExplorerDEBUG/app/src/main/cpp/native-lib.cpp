#include <jni.h>
#include <thread>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <android/log.h>
#include "Include/BNMResolve.hpp"
#include "extern/BNM-Android/src/private/Internals.hpp"
#include "Include/httplib.h"
#include "explorer/html_page.hpp"
#include "explorer/luabnm.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <fstream>

#include <dirent.h>
#include <sys/stat.h>

// Funcție pentru gestionarea folderului de scripturi
// Funcție sigură pentru gestionarea folderului de scripturi pe Android
static std::string GetScriptDir() {
    static std::string dir = "";
    if (dir.empty()) {
        BNM::AttachIl2Cpp();
        try {
            Class appCls("UnityEngine", "Application");
            if (appCls.IsValid()) {
                Method<BNM::Structures::Mono::String*> getPath(appCls.GetMethod("get_persistentDataPath"));
                if (getPath.IsValid()) {
                    auto* s = getPath();
                    if (s) dir = s->str() + "/LuaBNM/";
                }
            }
        } catch(...) {}
        
        // Fallback în caz că API-ul Unity eșuează
        if (dir.empty()) dir = "/sdcard/Download/LuaBNM/";
        
        mkdir(dir.c_str(), 0777); // Creează folderul
    }
    return dir;
}



#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "BNMExplorer", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BNMExplorer", __VA_ARGS__)

using namespace BNM;
using namespace BNM::IL2CPP;
using namespace BNM::Structures::Unity;

static int g_port = 8080;

static double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void AttachThread() {
    BNM::AttachIl2Cpp();
}

static std::string jsEsc(const char* s) {
    if (!s) return "";
    std::string o;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
        else                 o += *p;
    }
    return o;
}

static std::string jsEsc(const std::string& s) { return jsEsc(s.c_str()); }

static std::string typeName(const Il2CppType* t) {
    if (!t) return "?";
    Class c(t);
    if (!c.IsValid()) return "?";
    auto* r = c.GetClass();
    if (!r || !r->name) return "?";
    std::string n = r->name;
    if (r->namespaze && strlen(r->namespaze) > 0)
        n = std::string(r->namespaze) + "." + n;
    return n;
}

struct LoopEntry {
    std::string addr;
    std::string asm_;
    std::string ns;
    std::string cls;
    std::string name;
    std::string ftype;
    bool isProp;
    std::string val;
    int intervalMs;
    std::atomic<bool> active{true};
};

static std::mutex g_loopMtx;
static std::map<std::string, std::shared_ptr<LoopEntry>> g_loops;

struct LogEntry {
    std::string msg;
    int level;
    double timestamp;
};
static std::mutex g_logMtx;
static std::vector<LogEntry> g_logBuffer;
static const size_t LOG_MAX = 500;
static std::atomic<bool> g_logHooked{false};

static void addLogEntry(const std::string& msg, int level) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logBuffer.size() >= LOG_MAX) g_logBuffer.erase(g_logBuffer.begin());
    g_logBuffer.push_back({msg, level, nowSeconds()});
}

static void hookDebugLog() {
    if (g_logHooked.exchange(true)) return;
    try {
        Class debugCls("UnityEngine", "Debug");
        if (!debugCls.IsValid()) return;
    } catch(...) {}
}

struct CatcherArg {
    std::string type;
    std::string val;
};

struct CatcherCall {
    double ts;
    std::string instance;
    std::vector<CatcherArg> args;
    std::string ret;
};

struct CatcherEntry {
    std::string id;
    std::string asm_;
    std::string ns;
    std::string cls;
    std::string method;
    std::string retType;
    std::vector<std::string> paramTypes;
    std::vector<std::string> paramNames;
    std::atomic<bool> active{true};
    std::mutex callsMtx;
    std::vector<CatcherCall> calls;
    void* origPtr = nullptr;
    int slotIdx = -1;
    static const size_t MAX_CALLS = 100;
};

static std::mutex g_catcherMtx;
static std::map<std::string, std::shared_ptr<CatcherEntry>> g_catchers;

static std::string logsJson() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& e : g_logBuffer) {
        if (!first) j << ","; first = false;
        j << "{\"msg\":\"" << jsEsc(e.msg) << "\",\"level\":" << e.level << ",\"t\":" << std::fixed << e.timestamp << "}";
    }
    j << "]";
    return j.str();
}

static std::string jsonVal(const std::string& json, const std::string& key) {
    std::string qk = "\"" + key + "\":";
    auto pos = json.find(qk);
    if (pos == std::string::npos) return "";
    pos += qk.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos < json.length() && json[pos] == '"') {
        pos++;
        size_t end = pos;
        bool esc = false;
        while (end < json.length()) {
            if (json[end] == '\\' && !esc) esc = true;
            else if (json[end] == '"' && !esc) break;
            else esc = false;
            end++;
        }
        if (end >= json.length()) return "";
        std::string raw = json.substr(pos, end - pos);
        std::string out;
        for (size_t i = 0; i < raw.length(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.length()) {
                char n = raw[i+1];
                if      (n == '"')  { out += '"';  i++; }
                else if (n == '\\') { out += '\\'; i++; }
                else if (n == 'n')  { out += '\n'; i++; }
                else if (n == 'r')  { out += '\r'; i++; }
                else out += raw[i];
            } else out += raw[i];
        }
        return out;
    }
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) return "";
    std::string v = json.substr(pos, end - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    return v;
}

static MethodBase findObjsMethod() {
    static MethodBase m;
    if (m.IsValid()) return m;
    Class oc("UnityEngine", "Object");
    for (auto& mt : oc.GetMethods(false)) {
        auto* mi = mt.GetInfo();
        if (mi && mi->name && strcmp(mi->name, "FindObjectsOfType") == 0 && mi->parameters_count == 1)
            if (mi->parameters[0] && strstr(typeName(mi->parameters[0]).c_str(), "Type"))
            { m = mt; break; }
    }
    return m;
}

static Il2CppObject* sysTypeOf(const std::string& aqn) {
    Class tc("System", "Type", Image("mscorlib.dll"));
    if (!tc.IsValid()) return nullptr;
    Method<Il2CppObject*> gt(tc.GetMethod("GetType", 1));
    return gt.IsValid() ? gt(CreateMonoString(aqn.c_str())) : nullptr;
}

static bool isObjectAlive(Il2CppObject* obj) {
    if (!obj) return false;
    try {
        Method<bool> m(Class("UnityEngine", "Object").GetMethod("op_Implicit", 1));
        if (m.IsValid()) return m(obj);
        auto* k = obj->klass;
        if (!k || !k->name) return false;
        return true;
    } catch(...) { return false; }
}

static Il2CppObject* resolveComponentByAddr(uintptr_t addr) {
    if (!addr) return nullptr;
    Il2CppObject* obj = (Il2CppObject*)addr;
    if (!isObjectAlive(obj)) return nullptr;
    return obj;
}

static std::string assembliesJson() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& img : Image::GetImages()) {
        auto* d = img.GetInfo();
        if (!img.IsValid() || !d || !d->name) continue;
        if (!first) j << ",";
        first = false;
        j << "\"" << jsEsc(d->name) << "\"";
    }
    j << "]";
    return j.str();
}

static std::string classesJson(const std::string& asm_) {
    Image img(asm_);
    if (!img.IsValid()) return "[]";
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& cls : img.GetClasses(false)) {
        auto* r = cls.GetClass();
        if (!cls.IsValid() || !r || !r->name) continue;
        if (!first) j << ",";
        first = false;
        std::string t = "class";
        if (r->enumtype) t = "enum";
        else if (r->byval_arg.valuetype) t = "struct";
        else if (r->flags & 0x20) t = "interface";
        j << "{\"name\":\"" << jsEsc(r->name) << "\",\"ns\":\"" << jsEsc(r->namespaze ? r->namespaze : "") << "\",\"t\":\"" << t << "\"}";
    }
    j << "]";
    return j.str();
}

static std::string allClassesJson() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& img : Image::GetImages()) {
        auto* d = img.GetInfo();
        if (!img.IsValid() || !d || !d->name) continue;
        std::string a = d->name;
        for (auto& cls : img.GetClasses(false)) {
            auto* r = cls.GetClass();
            if (!cls.IsValid() || !r || !r->name) continue;
            if (!first) j << ",";
            first = false;
            std::string t = "class";
            if (r->enumtype) t = "enum";
            else if (r->byval_arg.valuetype) t = "struct";
            else if (r->flags & 0x20) t = "interface";
            j << "{\"name\":\"" << jsEsc(r->name) << "\",\"ns\":\"" << jsEsc(r->namespaze ? r->namespaze : "") << "\",\"t\":\"" << t << "\",\"a\":\"" << jsEsc(a) << "\"}";
        }
    }
    j << "]";
    return j.str();
}

static std::string classDetailJson(const std::string& a, const std::string& ns, const std::string& cn) {
    Class cls(ns.c_str(), cn.c_str(), Image(a.c_str()));
    if (!cls.IsValid()) return "{}";
    auto* r = cls.GetClass();
    if (!r) return "{}";

    std::ostringstream j;
    j << "{\"name\":\"" << jsEsc(cn) << "\",\"ns\":\"" << jsEsc(ns) << "\",\"asm\":\"" << jsEsc(a) << "\",";
    j << (r->parent && r->parent->name ? "\"parent\":\"" + jsEsc(r->parent->name) + "\"," : "\"parent\":null,");

    j << "\"fields\":[";
    bool first = true;
    for (auto& f : cls.GetFields(false)) {
        auto* fi = f.GetInfo();
        if (!f.IsValid() || !fi || !fi->name) continue;
        if (!first) j << ",";
        first = false;
        j << "{\"name\":\"" << jsEsc(fi->name) << "\",\"type\":\"" << jsEsc(typeName(fi->type)) << "\",\"s\":" << (fi->type && (fi->type->attrs & 0x10) ? "true" : "false") << ",\"off\":" << fi->offset << "}";
    }

    j << "],\"methods\":[";
    first = true;
    for (auto& m : cls.GetMethods(false)) {
        auto* mi = m.GetInfo();
        if (!m.IsValid() || !mi || !mi->name) continue;
        if (!first) j << ",";
        first = false;
        char ab[32] = {};
        if (mi->methodPointer) snprintf(ab, sizeof(ab), "%llX", (unsigned long long)(uintptr_t)mi->methodPointer);
        j << "{\"name\":\"" << jsEsc(mi->name) << "\",\"ret\":\"" << jsEsc(typeName(mi->return_type)) << "\",\"s\":" << ((mi->flags & 0x10) ? "true" : "false") << ",\"addr\":\"" << ab << "\",\"params\":[";
        for (int i = 0; i < (int)mi->parameters_count; i++) {
            if (i > 0) j << ",";
            j << "{\"n\":\"arg" << i << "\",\"t\":\"" << jsEsc(mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?") << "\"}";
        }
        j << "]}";
    }

    j << "],\"props\":[";
    first = true;
    for (auto& p : cls.GetProperties(false)) {
        auto* pi = p._data;
        if (!p.IsValid() || !pi || !pi->name) continue;
        if (!first) j << ",";
        first = false;
        j << "{\"name\":\"" << jsEsc(pi->name) << "\",\"type\":\"" << jsEsc(pi->get && pi->get->return_type ? typeName(pi->get->return_type) : "?") << "\",\"g\":" << (pi->get ? "true" : "false") << ",\"s\":" << (pi->set ? "true" : "false") << "}";
    }
    j << "]}";
    return j.str();
}

struct InvokeResult { bool ok; std::string val, err; };

static InvokeResult invokeMethod(const std::string& body) {
    AttachThread();
    InvokeResult res = {false, "", ""};
    std::string a = jsonVal(body, "asm"), ns = jsonVal(body, "ns"), cn = jsonVal(body, "cls"), mn = jsonVal(body, "method");
    bool isStatic = jsonVal(body, "static") == "true" || jsonVal(body, "static") == "1";
    uintptr_t instAddr = (uintptr_t)strtoull(jsonVal(body, "instance").c_str(), nullptr, 16);

    std::vector<std::string> argT, argV;
    auto ap = body.find("\"args\":[");
    if (ap != std::string::npos) {
        ap += 8;
        auto ae = body.find("]", ap);
        if (ae != std::string::npos) {
            std::string ab = body.substr(ap, ae - ap);
            size_t p = 0;
            while (p < ab.size()) {
                auto ob = ab.find("{", p), cb = ab.find("}", ob);
                if (ob == std::string::npos || cb == std::string::npos) break;
                std::string e = "{" + ab.substr(ob + 1, cb - ob - 1) + "}";
                argT.push_back(jsonVal(e, "t"));
                argV.push_back(jsonVal(e, "v"));
                p = cb + 1;
            }
        }
    }

    if (a.empty() || cn.empty() || mn.empty()) { res.err = "Missing asm/cls/method"; return res; }

    Class cls(ns.c_str(), cn.c_str(), Image(a.c_str()));
    if (!cls.IsValid()) { res.err = "Class not found"; return res; }

    MethodBase mb = cls.GetMethod(mn.c_str(), (int)argT.size());
    if (!mb.IsValid()) mb = cls.GetMethod(mn.c_str());
    if (!mb.IsValid()) {
        for (Class cur = cls.GetParent(); cur.IsValid() && cur.GetClass() && !mb.IsValid(); cur = cur.GetParent()) {
            mb = cur.GetMethod(mn.c_str(), (int)argT.size());
            if (!mb.IsValid()) mb = cur.GetMethod(mn.c_str());
        }
    }
    if (!mb.IsValid()) { res.err = "Method not found"; return res; }
    auto* mi = mb.GetInfo();
    if (!mi || !mi->methodPointer) { res.err = "No pointer"; return res; }

    std::vector<void*> runtimeArgs;
    std::vector<int32_t> vi;
    std::vector<int64_t> vi64;
    std::vector<float> vf;
    std::vector<double> vd;
    std::vector<uint8_t> vb;
    std::vector<int16_t> vs16;
    std::vector<Vector3> vv3;
    std::vector<Vector2> vv2;
    std::vector<Color> vc;
    std::vector<Vector4> vv4;

    for (size_t i = 0; i < argT.size(); i++) {
        auto& t = argT[i]; auto& v = argV[i];
        if (t=="System.Int32"||t=="Int32"||t=="int") {
            vi.push_back(std::stoi(v.empty()?"0":v)); runtimeArgs.push_back(&vi.back());
        } else if (t=="System.Int64"||t=="Int64"||t=="long") {
            vi64.push_back(std::stoll(v.empty()?"0":v)); runtimeArgs.push_back(&vi64.back());
        } else if (t=="System.Single"||t=="Single"||t=="float") {
            vf.push_back(std::stof(v.empty()?"0":v)); runtimeArgs.push_back(&vf.back());
        } else if (t=="System.Double"||t=="Double"||t=="double") {
            vd.push_back(std::stod(v.empty()?"0":v)); runtimeArgs.push_back(&vd.back());
        } else if (t=="System.Boolean"||t=="Boolean"||t=="bool") {
            vb.push_back((uint8_t)(v=="true"||v=="1"?1:0)); runtimeArgs.push_back(&vb.back());
        } else if (t=="System.Byte"||t=="Byte"||t=="byte") {
            vb.push_back((uint8_t)std::stoul(v.empty()?"0":v)); runtimeArgs.push_back(&vb.back());
        } else if (t=="System.Int16"||t=="Int16"||t=="short") {
            vs16.push_back((int16_t)std::stoi(v.empty()?"0":v)); runtimeArgs.push_back(&vs16.back());
        } else if (t=="UnityEngine.Vector3"||t=="Vector3") {
            float x=0,y=0,z=0; sscanf(v.c_str(),"[%f,%f,%f]",&x,&y,&z); vv3.push_back({x,y,z}); runtimeArgs.push_back(&vv3.back());
        } else if (t=="UnityEngine.Vector2"||t=="Vector2") {
            float x=0,y=0; sscanf(v.c_str(),"[%f,%f]",&x,&y); vv2.push_back({x,y}); runtimeArgs.push_back(&vv2.back());
        } else if (t=="UnityEngine.Color"||t=="Color") {
            float r=1,g=1,b=1,aa=1; sscanf(v.c_str(),"[%f,%f,%f,%f]",&r,&g,&b,&aa); vc.push_back({r,g,b,aa}); runtimeArgs.push_back(&vc.back());
        } else if (t=="UnityEngine.Vector4"||t=="Vector4"||t=="UnityEngine.Quaternion"||t=="Quaternion") {
            float x=0,y=0,z=0,w=0; sscanf(v.c_str(),"[%f,%f,%f,%f]",&x,&y,&z,&w); vv4.push_back({x,y,z,w}); runtimeArgs.push_back(&vv4.back());
        } else if (t=="System.String"||t=="String"||t=="string") {
            runtimeArgs.push_back(CreateMonoString(v));
        } else {
            runtimeArgs.push_back((void*)strtoull(v.empty()?"0":v.c_str(), nullptr, 16));
        }
    }

    void* inst = isStatic ? nullptr : (void*)instAddr;
    std::string rtn = typeName(mi->return_type);
    std::ostringstream out;

    Il2CppException* exc = nullptr;
    void* ret = BNM::Internal::il2cppMethods.il2cpp_runtime_invoke(mi, inst, runtimeArgs.empty() ? nullptr : runtimeArgs.data(), &exc);

    if (exc) {
        BNM::Structures::Mono::String* excMsg = nullptr;
        try {
            auto excCls = Class(((Il2CppObject*)exc)->klass);
            if (excCls.IsValid()) {
                auto msgMethod = excCls.GetMethod("get_Message", 0);
                if (msgMethod.IsValid()) {
                    auto* msgMi = msgMethod.GetInfo();
                    if (msgMi && msgMi->methodPointer) {
                        Il2CppException* inner = nullptr;
                        auto* msgRet = (Il2CppObject*)BNM::Internal::il2cppMethods.il2cpp_runtime_invoke(msgMi, exc, nullptr, &inner);
                        if (msgRet && !inner) excMsg = (BNM::Structures::Mono::String*)msgRet;
                    }
                }
            }
        } catch(...) {}
        res.err = excMsg ? excMsg->str() : "IL2CPP exception";
        return res;
    }

    if (rtn=="System.Void"||rtn=="Void"||rtn=="void"||rtn=="?") {
        out << "void";
    } else if (rtn=="System.Single"||rtn=="Single"||rtn=="float") {
        float v = ret ? *(float*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Double"||rtn=="Double"||rtn=="double") {
        double v = ret ? *(double*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Int32"||rtn=="Int32"||rtn=="int") {
        int v = ret ? *(int*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Int64"||rtn=="Int64"||rtn=="long") {
        int64_t v = ret ? *(int64_t*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Boolean"||rtn=="Boolean"||rtn=="bool") {
        bool v = ret ? *(bool*)((uint8_t*)ret + sizeof(Il2CppObject)) : false;
        out << (v?"true":"false");
    } else if (rtn=="System.Byte"||rtn=="Byte"||rtn=="byte") {
        uint8_t v = ret ? *(uint8_t*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << (int)v;
    } else if (rtn=="UnityEngine.Vector3"||rtn=="Vector3") {
        Vector3 v = ret ? *(Vector3*)((uint8_t*)ret + sizeof(Il2CppObject)) : Vector3{0,0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f, %.4f]",v.x,v.y,v.z); out<<buf;
    } else if (rtn=="UnityEngine.Vector2"||rtn=="Vector2") {
        Vector2 v = ret ? *(Vector2*)((uint8_t*)ret + sizeof(Il2CppObject)) : Vector2{0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f]",v.x,v.y); out<<buf;
    } else if (rtn=="UnityEngine.Color"||rtn=="Color") {
        Color v = ret ? *(Color*)((uint8_t*)ret + sizeof(Il2CppObject)) : Color{0,0,0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f, %.4f, %.4f]",v.r,v.g,v.b,v.a); out<<buf;
    } else if (rtn=="System.String"||rtn=="String"||rtn=="string") {
        auto* s = (BNM::Structures::Mono::String*)ret;
        out << (s ? s->str() : "null");
    } else {
        char buf[64]; snprintf(buf,sizeof(buf),"0x%llX",(unsigned long long)(uintptr_t)ret);
        out << rtn << " @ " << buf;
    }
    res.ok = true; res.val = out.str();
    return res;
}

static std::string instancesJson(const std::string& a, const std::string& ns, const std::string& cn) {
    AttachThread();
    std::string full = ns.empty() ? cn : ns + "." + cn;
    std::string aNoExt = a;
    auto dp = aNoExt.find(".dll");
    if (dp != std::string::npos) aNoExt = aNoExt.substr(0, dp);

    Il2CppObject* st = sysTypeOf(full + ", " + aNoExt);
    if (!st) st = sysTypeOf(full);
    if (!st) return "{\"error\":\"Class not found / System.Type missing\"}";

    auto fm = findObjsMethod();
    if (!fm.IsValid()) return "{\"error\":\"FindObjectsOfType missing\"}";

    std::ostringstream j;
    j << "{\"instances\":[";
    try {
        auto* arr = Method<Array<Il2CppObject*>*>(fm)(st);
        if (arr && arr->capacity > 0) {
            bool first = true;
            Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
            for (int i = 0; i < arr->capacity; i++) {
                Il2CppObject* obj = arr->m_Items[i];
                if (!obj) continue;
                if (!first) j << ",";
                first = false;
                std::string name = "obj";
                if (gn.IsValid()) { auto* s = gn[obj](); if (s) name = s->str(); }
                char addr[32]; snprintf(addr, sizeof(addr), "%llX", (unsigned long long)(uintptr_t)obj);
                j << "{\"addr\":\"" << addr << "\",\"name\":\"" << jsEsc(name) << "\"}";
            }
        }
    } catch(...) {}
    j << "]}";
    return j.str();
}

static std::string sceneJson() {
    AttachThread();
    auto fm = findObjsMethod();
    if (!fm.IsValid()) return "[]";

    Il2CppObject* st = sysTypeOf("UnityEngine.GameObject, UnityEngine.CoreModule");
    if (!st) st = sysTypeOf("UnityEngine.GameObject, UnityEngine");
    if (!st) st = sysTypeOf("UnityEngine.GameObject");
    if (!st) return "[]";

    Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
    Method<bool> ga(Class("UnityEngine","GameObject").GetMethod("get_activeSelf"));
    Method<Il2CppObject*> gt(Class("UnityEngine","GameObject").GetMethod("get_transform"));
    Method<Il2CppObject*> tp(Class("UnityEngine","Transform").GetMethod("get_parent"));
    Method<Il2CppObject*> cg(Class("UnityEngine","Component").GetMethod("get_gameObject"));

    std::ostringstream j;
    j << "[";
    try {
        auto* arr = Method<Array<Il2CppObject*>*>(fm)(st);
        if (arr && arr->capacity > 0) {
            bool first = true;
            for (int i = 0; i < arr->capacity; i++) {
                Il2CppObject* go = arr->m_Items[i];
                if (!go) continue;
                std::string name = "Unknown";
                bool active = false;
                if (gn.IsValid()) { auto* s = gn[go](); if (s) name = s->str(); }
                if (ga.IsValid()) active = ga[go]();
                uintptr_t par = 0;
                if (gt.IsValid() && tp.IsValid() && cg.IsValid()) {
                    Il2CppObject* tr = gt[go]();
                    if (tr) { Il2CppObject* pt = tp[tr](); if (pt) { Il2CppObject* pg = cg[pt](); if (pg) par = (uintptr_t)pg; } }
                }
                if (!first) j << ",";
                first = false;
                j << "{\"addr\":\"" << std::hex << (uintptr_t)go << "\",\"name\":\"" << jsEsc(name) << "\",\"active\":" << (active?"true":"false") << ",\"parent\":\"" << std::hex << par << "\"}";
            }
        }
    } catch(...) {}
    j << "]";
    return j.str();
}

static std::string readField(const std::string& ft, Class& cls, Il2CppObject* inst, const std::string& name, MethodBase* getter, bool isStatic, bool& ok) {
    ok = false;
    if (!inst && !isStatic) return "";
    try {
        if (ft=="System.Single"||ft=="Single"||ft=="float") {
            float v = 0;
            if (getter) { Method<float> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<float> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Double"||ft=="Double"||ft=="double") {
            double v = 0;
            if (getter) { Method<double> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<double> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int32"||ft=="Int32"||ft=="int") {
            int v = 0;
            if (getter) { Method<int> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt32"||ft=="UInt32"||ft=="uint") {
            uint32_t v = 0;
            if (getter) { Method<uint32_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint32_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int64"||ft=="Int64"||ft=="long") {
            int64_t v = 0;
            if (getter) { Method<int64_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt64"||ft=="UInt64"||ft=="ulong") {
            uint64_t v = 0;
            if (getter) { Method<uint64_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int16"||ft=="Int16"||ft=="short") {
            int16_t v = 0;
            if (getter) { Method<int16_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt16"||ft=="UInt16"||ft=="ushort") {
            uint16_t v = 0;
            if (getter) { Method<uint16_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Byte"||ft=="Byte"||ft=="byte") {
            uint8_t v = 0;
            if (getter) { Method<uint8_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.SByte"||ft=="SByte"||ft=="sbyte") {
            int8_t v = 0;
            if (getter) { Method<int8_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Boolean"||ft=="Boolean"||ft=="bool") {
            bool v = false;
            if (getter) { Method<bool> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<bool> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return v ? "true" : "false";
        } else if (ft=="System.String"||ft=="String"||ft=="string") {
            BNM::Structures::Mono::String* s = nullptr;
            if (getter) { Method<BNM::Structures::Mono::String*> m(*getter); if(!isStatic) m.SetInstance(inst); s = m(); }
            else { Field<BNM::Structures::Mono::String*> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);s=f();} }
            ok = true; return s ? "\"" + jsEsc(s->str()) + "\"" : "\"\"";
        } else if (ft=="UnityEngine.Vector3"||ft=="Vector3") {
            Vector3 v = {0,0,0};
            if (getter) { Method<Vector3> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector3> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return "[" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "]";
        } else if (ft=="UnityEngine.Color"||ft=="Color") {
            Color c = {0,0,0,0};
            if (getter) { Method<Color> m(*getter); if(!isStatic) m.SetInstance(inst); c = m(); }
            else { Field<Color> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);c=f();} }
            ok = true; return "[" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a) + "]";
        } else if (ft=="UnityEngine.Vector2"||ft=="Vector2") {
            Vector2 v = {0,0};
            if (getter) { Method<Vector2> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector2> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return "[" + std::to_string(v.x) + "," + std::to_string(v.y) + "]";
        } else if (ft=="UnityEngine.Vector4"||ft=="Vector4"||ft=="UnityEngine.Quaternion"||ft=="Quaternion") {
            Vector4 v = {0,0,0,0};
            if (getter) { Method<Vector4> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector4> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::string("{\"x\":") + std::to_string(v.x) + ",\"y\":" + std::to_string(v.y) + ",\"z\":" + std::to_string(v.z) + ",\"w\":" + std::to_string(v.w) + "}";
        } else if (ft=="UnityEngine.LayerMask"||ft=="LayerMask") {
            int v = 0;
            if (getter) { Method<int> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="UnityEngine.Rect"||ft=="Rect") {
            Rect v = {0,0,0,0};
            if (getter) { Method<Rect> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Rect> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true;
            return std::string("{\"x\":") + std::to_string(v.x) + ",\"y\":" + std::to_string(v.y) + ",\"w\":" + std::to_string(v.w) + ",\"h\":" + std::to_string(v.h) + "}";
        }
    } catch(...) {}
    return "";
}

static void writeField(const std::string& ft, Class& cls, Il2CppObject* inst, const std::string& name, MethodBase* setter, const std::string& v, bool isStatic = false) {
    if (!inst && !isStatic) return;
    try {
        if (ft=="System.Single"||ft=="Single"||ft=="float") {
            float val = std::stof(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<float> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Double"||ft=="Double"||ft=="double") {
            double val = std::stod(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<double> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int32"||ft=="Int32"||ft=="int") {
            int val = std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt32"||ft=="UInt32"||ft=="uint") {
            uint32_t val = (uint32_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint32_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int64"||ft=="Int64"||ft=="long") {
            int64_t val = std::stoll(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt64"||ft=="UInt64"||ft=="ulong") {
            uint64_t val = std::stoull(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int16"||ft=="Int16"||ft=="short") {
            int16_t val = (int16_t)std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt16"||ft=="UInt16"||ft=="ushort") {
            uint16_t val = (uint16_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Byte"||ft=="Byte"||ft=="byte") {
            uint8_t val = (uint8_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.SByte"||ft=="SByte"||ft=="sbyte") {
            int8_t val = (int8_t)std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Boolean"||ft=="Boolean"||ft=="bool") {
            bool val = v=="true"||v=="1";
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<bool> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.String"||ft=="String"||ft=="string") {
            auto* s = CreateMonoString(v.c_str());
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(s); }
            else { Field<BNM::Structures::Mono::String*> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=s;} }
        } else if (ft=="UnityEngine.Vector3"||ft=="Vector3") {
            float x=0,y=0,z=0; sscanf(v.c_str(),"[%f,%f,%f]",&x,&y,&z); Vector3 val={x,y,z};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector3> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Color"||ft=="Color") {
            float r=1,g=1,b=1,a=1; sscanf(v.c_str(),"[%f,%f,%f,%f]",&r,&g,&b,&a); Color val={r,g,b,a};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Color> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Vector2"||ft=="Vector2") {
            float x=0,y=0; sscanf(v.c_str(),"[%f,%f]",&x,&y); Vector2 val={x,y};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector2> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Vector4"||ft=="Vector4"||ft=="UnityEngine.Quaternion"||ft=="Quaternion") {
            float x=0,y=0,z=0,w=0;
            sscanf(v.c_str(),"{\"x\":%f,\"y\":%f,\"z\":%f,\"w\":%f}",&x,&y,&z,&w);
            Vector4 val={x,y,z,w};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector4> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.LayerMask"||ft=="LayerMask") {
            int val = std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Rect"||ft=="Rect") {
            float x=0,y=0,w=0,h=0;
            sscanf(v.c_str(),"{\"x\":%f,\"y\":%f,\"w\":%f,\"h\":%f}",&x,&y,&w,&h);
            Rect val={x,y,w,h};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Rect> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        }
    } catch(...) {}
}

static std::string goInfoJson(uintptr_t addr) {
    AttachThread();
    if (!addr) return "{}";
    Il2CppObject* go = (Il2CppObject*)addr;
    if (!isObjectAlive(go)) return "{\"stale\":true}";
    std::string name = "Unknown";
    bool active = false;

    Class goCls("UnityEngine","GameObject");
    Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
    Method<bool> ga(goCls.GetMethod("get_activeSelf"));
    Method<Il2CppObject*> gt(goCls.GetMethod("get_transform"));

    try {
        if (gn.IsValid()) { auto* s=gn[go](); if(s) name=s->str(); }
        if (ga.IsValid()) active = ga[go]();
    } catch(...) {}

    std::ostringstream j;
    j << "{\"addr\":\"" << std::hex << addr << "\",\"name\":\"" << jsEsc(name) << "\",\"active\":" << (active?"true":"false") << ",\"transform\":{";
    try {
        if (gt.IsValid()) {
            Il2CppObject* tr = gt[go]();
            if (tr) {
                Method<Vector3> gp(Class("UnityEngine","Transform").GetMethod("get_localPosition"));
                Method<Vector3> gr(Class("UnityEngine","Transform").GetMethod("get_localEulerAngles"));
                Method<Vector3> gs(Class("UnityEngine","Transform").GetMethod("get_localScale"));
                Vector3 p=gp.IsValid()?gp[tr]():Vector3{0,0,0};
                Vector3 r=gr.IsValid()?gr[tr]():Vector3{0,0,0};
                Vector3 s=gs.IsValid()?gs[tr]():Vector3{0,0,0};
                j << "\"addr\":\"" << std::hex << (uintptr_t)tr << "\",\"p\":[" << p.x << "," << p.y << "," << p.z << "],\"r\":[" << r.x << "," << r.y << "," << r.z << "],\"s\":[" << s.x << "," << s.y << "," << s.z << "]";
            }
        }
    } catch(...) {}
    j << "},\"scripts\":[";

    try {
        Class componentCls("UnityEngine", "Component");
        Il2CppObject* compReflType = nullptr;
        if (componentCls.IsValid()) compReflType = (Il2CppObject*)componentCls.GetMonoType();
        if (!compReflType) compReflType = sysTypeOf("UnityEngine.Component, UnityEngine.CoreModule");
        if (!compReflType) compReflType = sysTypeOf("UnityEngine.Component");
        Array<Il2CppObject*>* comps = nullptr;
        if (!comps && compReflType) {
            for (auto& mt : goCls.GetMethods(true)) {
                auto* mi = mt.GetInfo();
                if (!mi || !mi->name || mi->is_generic) continue;
                if (strcmp(mi->name, "GetComponents") != 0) continue;
                if (mi->parameters_count == 1) {
                    try {
                        Method<Array<Il2CppObject*>*> m(mt);
                        comps = m[go](compReflType);
                    } catch(...) { LOGE("goInfoJson: GetComponents(Type) threw"); }
                    if (comps && comps->capacity > 0) break;
                    comps = nullptr;
                }
            }
        }
        if (!comps && compReflType) {
            for (auto& mt : goCls.GetMethods(true)) {
                auto* mi = mt.GetInfo();
                if (!mi || !mi->name) continue;
                if (strcmp(mi->name, "GetComponentsInternal") != 0) continue;
                LOGD("goInfoJson: found GetComponentsInternal with %d params", mi->parameters_count);
                if (mi->parameters_count >= 2) {
                    try {
                        if (mi->parameters_count == 6) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, true, false, true, false, nullptr);
                            LOGD("goInfoJson: GetComponentsInternal(6) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        } else if (mi->parameters_count == 5) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, true, false, true, false);
                            LOGD("goInfoJson: GetComponentsInternal(5) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        } else if (mi->parameters_count == 4) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, false, true, false);
                            LOGD("goInfoJson: GetComponentsInternal(4) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        }
                    } catch(...) { LOGE("goInfoJson: GetComponentsInternal threw"); }
                    if (comps && comps->capacity > 0) break;
                    comps = nullptr;
                }
            }
        }
        if (!comps) {
            LOGD("goInfoJson: falling back to FindObjectsOfType");
            Class monoCls("UnityEngine", "MonoBehaviour");
            Il2CppObject* monoType = nullptr;
            if (monoCls.IsValid()) monoType = (Il2CppObject*)monoCls.GetMonoType();
            if (!monoType) monoType = sysTypeOf("UnityEngine.MonoBehaviour, UnityEngine.CoreModule");
            if (!monoType) monoType = sysTypeOf("UnityEngine.MonoBehaviour");

            auto fom = findObjsMethod();
            Method<Il2CppObject*> compGetGO(Class("UnityEngine","Component").GetMethod("get_gameObject"));

            if (fom.IsValid() && monoType && compGetGO.IsValid()) {
                auto* allMonos = Method<Array<Il2CppObject*>*>(fom)(monoType);
                LOGD("goInfoJson: FindObjectsOfType(MonoBehaviour) count=%d", allMonos ? (int)allMonos->capacity : -1);
                if (allMonos && allMonos->capacity > 0) {
                    for (int i = 0; i < allMonos->capacity; i++) {
                        Il2CppObject* c = allMonos->m_Items[i];
                        if (!c) continue;
                        try {
                            Il2CppObject* cgo = compGetGO[c]();
                            if ((uintptr_t)cgo != addr) allMonos->m_Items[i] = nullptr;
                        } catch(...) { allMonos->m_Items[i] = nullptr; }
                    }
                    comps = allMonos;
                }
            }
        }

        LOGD("goInfoJson: final comps=%p, count=%d", comps, comps ? (int)comps->capacity : -1);

        static const char* kStopAtComponent[] = {
                "Component", "Object", nullptr
        };
        static const char* kSkipProps[] = {
                "isActiveAndEnabled", "transform", "gameObject", "tag", "name",
                "hideFlags", "rigidbody", "rigidbody2D", "camera", "light",
                "animation", "constantForce", "renderer", "audio", "networkView",
                "collider", "collider2D", "hingeJoint", "particleSystem", nullptr
        };

        auto isStopClass = [](const char* n) -> bool {
            for (auto** s = kStopAtComponent; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };
        auto isSkipProp = [](const char* n) -> bool {
            for (auto** s = kSkipProps; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };

        auto isKnownBuiltinBase = [](const char* n) -> bool {
            static const char* builtins[] = {
                    "MonoBehaviour", "Behaviour",
                    "Collider", "Collider2D",
                    "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "TerrainCollider", "WheelCollider",
                    "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "EdgeCollider2D", "CapsuleCollider2D", "CompositeCollider2D",
                    "Rigidbody", "Rigidbody2D",
                    "Renderer", "MeshRenderer", "SkinnedMeshRenderer", "SpriteRenderer", "LineRenderer", "TrailRenderer", "ParticleSystemRenderer",
                    "MeshFilter",
                    "Animator", "Animation",
                    "AudioSource", "AudioListener",
                    "Camera", "Light", "LensFlare", "Projector",
                    "Canvas", "CanvasGroup", "CanvasRenderer",
                    "RectTransform",
                    "Text", "Image", "RawImage", "Button", "Toggle", "Slider", "Scrollbar", "Dropdown", "InputField", "ScrollRect", "Mask", "RectMask2D",
                    "TMP_Text", "TextMeshPro", "TextMeshProUGUI",
                    "NavMeshAgent", "NavMeshObstacle",
                    "ParticleSystem",
                    "Joint", "HingeJoint", "FixedJoint", "SpringJoint", "CharacterJoint", "ConfigurableJoint",
                    "Joint2D", "HingeJoint2D", "FixedJoint2D", "SpringJoint2D", "WheelJoint2D", "SliderJoint2D",
                    "CharacterController",
                    "LODGroup",
                    "OcclusionArea", "OcclusionPortal",
                    "NetworkView",
                    "ConstantForce",
                    nullptr
            };
            for (auto** s = builtins; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };

        if (comps && comps->capacity > 0) {
            bool firstScript = true;

            for (int i = 0; i < comps->capacity; i++) {
                Il2CppObject* comp = comps->m_Items[i];
                if (!comp || !comp->klass) continue;

                auto* klass = comp->klass;

                bool isSupported = false;
                std::string compCategory = "script";
                for (auto* cur = klass; cur; cur = cur->parent) {
                    if (!cur->name) continue;
                    if (strcmp(cur->name, "MonoBehaviour") == 0) { isSupported = true; compCategory = "script"; break; }
                    if (isKnownBuiltinBase(cur->name) && strcmp(cur->name, "MonoBehaviour") != 0 && strcmp(cur->name, "Behaviour") != 0) {
                        isSupported = true; compCategory = "builtin"; break;
                    }
                    if (strcmp(cur->name, "Component") == 0 || strcmp(cur->name, "Object") == 0) break;
                }
                if (!isSupported) continue;

                std::string compName = klass->name ? klass->name : "Unknown";
                std::string compNs = klass->namespaze ? klass->namespaze : "";
                char compAddr[32]; snprintf(compAddr, sizeof(compAddr), "%llX", (unsigned long long)(uintptr_t)comp);
                std::string compAsm = (klass->image && klass->image->name) ? klass->image->name : "";

                bool hasEnabledProp = false;
                bool enabled = false;
                for (auto* cur = klass; cur; cur = cur->parent) {
                    if (!cur->name) continue;
                    if (strcmp(cur->name, "Behaviour") == 0 || strcmp(cur->name, "Renderer") == 0 ||
                        strcmp(cur->name, "Collider") == 0 || strcmp(cur->name, "Collider2D") == 0) {
                        hasEnabledProp = true; break;
                    }
                    if (strcmp(cur->name, "Component") == 0) break;
                }
                if (hasEnabledProp) {
                    Method<bool> getEnabled(Class("UnityEngine","Behaviour").GetMethod("get_enabled"));
                    if (!getEnabled.IsValid()) {
                        Class compClsCheck(klass);
                        auto ep = compClsCheck.GetProperty("enabled");
                        if (ep.IsValid() && ep._data && ep._data->get) {
                            MethodBase egetter(ep._data->get);
                            Method<bool> m(egetter); m.SetInstance(comp);
                            try { enabled = m(); } catch(...) {}
                        }
                    } else {
                        try { enabled = getEnabled[comp](); } catch(...) {}
                    }
                }

                if (!firstScript) j << ",";
                firstScript = false;
                j << "{\"addr\":\"" << compAddr << "\",\"name\":\"" << jsEsc(compName) << "\",\"ns\":\"" << jsEsc(compNs) << "\",\"asm\":\"" << jsEsc(compAsm) << "\",\"category\":\"" << compCategory << "\",\"enabled\":" << (enabled?"true":"false") << ",\"fields\":[";

                LOGD("goInfoJson: found component '%s' category='%s' at %s", compName.c_str(), compCategory.c_str(), compAddr);

                try {
                    Class compCls(klass);
                    bool firstF = true;
                    std::vector<std::string> seen;

                    for (Class cur = compCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                        auto* cn_ = cur.GetClass()->name;
                        if (!cn_) break;
                        if (isStopClass(cn_)) break;

                        for (auto& f : cur.GetFields(false)) {
                            try {
                                auto* fi = f.GetInfo();
                                if (!fi || !fi->type || (fi->type->attrs & 0x10)) continue;
                                std::string fn = fi->name;
                                bool dup = false; for (auto& s : seen) if (s==fn) dup=true;
                                if (dup) continue; seen.push_back(fn);
                                std::string ft = typeName(fi->type);
                                bool ok = false;
                                std::string vs = readField(ft, cur, comp, fn, nullptr, false, ok);
                                if (ok) {
                                    if (!firstF) j << ","; firstF = false;
                                    j << "{\"name\":\"" << jsEsc(fn) << "\",\"type\":\"" << jsEsc(ft) << "\",\"val\":" << vs << ",\"isProp\":false,\"canWrite\":true}";
                                }
                            } catch(...) {}
                        }

                        for (auto& p : cur.GetProperties(false)) {
                            try {
                                auto* pi = p._data;
                                if (!pi || !pi->get) continue;
                                std::string pn = pi->name;
                                if (isSkipProp(pn.c_str())) continue;
                                bool dup = false; for (auto& s : seen) if (s==pn) dup=true;
                                if (dup) continue; seen.push_back(pn);
                                std::string pt = typeName(pi->get->return_type);
                                bool ok = false;
                                MethodBase getter(pi->get);
                                std::string vs = readField(pt, cur, comp, pn, &getter, false, ok);
                                if (ok) {
                                    if (!firstF) j << ","; firstF = false;
                                    j << "{\"name\":\"" << jsEsc(pn) << "\",\"type\":\"" << jsEsc(pt) << "\",\"val\":" << vs << ",\"isProp\":true,\"canWrite\":" << (pi->set?"true":"false") << "}";
                                }
                            } catch(...) {}
                        }
                    }
                } catch(...) {}
                j << "]}";
            }
        }
    } catch(...) {
        LOGE("goInfoJson: exception in script enumeration");
    }
    j << "]}";
    return j.str();
}

static void applyLoopEntry(const std::shared_ptr<LoopEntry>& e) {
    try {
        uintptr_t addr = (uintptr_t)strtoull(e->addr.c_str(), nullptr, 16);
        if (!addr) return;
        Il2CppObject* obj = resolveComponentByAddr(addr);
        if (!obj) { e->active = false; return; }
        for (Class cur(e->ns.c_str(), e->cls.c_str(), Image(e->asm_.c_str())); cur.IsValid(); cur = cur.GetParent()) {
            if (e->isProp) {
                auto p = cur.GetProperty(e->name.c_str());
                if (p.IsValid() && p._data && p._data->set) {
                    MethodBase s(p._data->set);
                    writeField(e->ftype, cur, obj, e->name, &s, e->val);
                    return;
                }
            } else {
                auto f = cur.GetField(e->name.c_str());
                if (f.IsValid()) { writeField(e->ftype, cur, obj, e->name, nullptr, e->val); return; }
            }
        }
    } catch(...) {}
}

static std::string loopListJson() {
    std::lock_guard<std::mutex> lk(g_loopMtx);
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& kv : g_loops) {
        if (!first) j << ","; first = false;
        auto& e = kv.second;
        j << "{\"id\":\"" << jsEsc(kv.first) << "\",\"cls\":\"" << jsEsc(e->cls) << "\",\"name\":\"" << jsEsc(e->name) << "\",\"val\":\"" << jsEsc(e->val) << "\",\"interval\":" << e->intervalMs << ",\"active\":" << (e->active?"true":"false") << "}";
    }
    j << "]";
    return j.str();
}

#define MAX_CATCHERS 64
static std::shared_ptr<CatcherEntry>* g_catcherSlots[MAX_CATCHERS] = {};
static int g_catcherSlotCount = 0;
static std::mutex g_catcherSlotMtx;
static void (*g_catcherOrigPtrs[MAX_CATCHERS])(Il2CppObject*) = {};

template<int N>
static void catcherHook(Il2CppObject* self) {
    std::shared_ptr<CatcherEntry>* slot = g_catcherSlots[N];
    if (slot && *slot && (*slot)->active) {
        CatcherCall call;
        call.ts = nowSeconds();
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)(uintptr_t)self);
        call.instance = buf;
        if (self && self->klass) {
            try {
                Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
                if (gn.IsValid()) { auto* s = gn[self](); if (s) call.instance += " (" + s->str() + ")"; }
            } catch(...) {}
        }
        std::lock_guard<std::mutex> lk((*slot)->callsMtx);
        if ((*slot)->calls.size() >= CatcherEntry::MAX_CALLS) (*slot)->calls.erase((*slot)->calls.begin());
        (*slot)->calls.push_back(std::move(call));
    }
    if (g_catcherOrigPtrs[N]) g_catcherOrigPtrs[N](self);
}

typedef void(*CatcherHookFn)(Il2CppObject*);
static CatcherHookFn g_catcherHookFns[MAX_CATCHERS] = {
        catcherHook<0>,catcherHook<1>,catcherHook<2>,catcherHook<3>,catcherHook<4>,
        catcherHook<5>,catcherHook<6>,catcherHook<7>,catcherHook<8>,catcherHook<9>,
        catcherHook<10>,catcherHook<11>,catcherHook<12>,catcherHook<13>,catcherHook<14>,
        catcherHook<15>,catcherHook<16>,catcherHook<17>,catcherHook<18>,catcherHook<19>,
        catcherHook<20>,catcherHook<21>,catcherHook<22>,catcherHook<23>,catcherHook<24>,
        catcherHook<25>,catcherHook<26>,catcherHook<27>,catcherHook<28>,catcherHook<29>,
        catcherHook<30>,catcherHook<31>,catcherHook<32>,catcherHook<33>,catcherHook<34>,
        catcherHook<35>,catcherHook<36>,catcherHook<37>,catcherHook<38>,catcherHook<39>,
        catcherHook<40>,catcherHook<41>,catcherHook<42>,catcherHook<43>,catcherHook<44>,
        catcherHook<45>,catcherHook<46>,catcherHook<47>,catcherHook<48>,catcherHook<49>,
        catcherHook<50>,catcherHook<51>,catcherHook<52>,catcherHook<53>,catcherHook<54>,
        catcherHook<55>,catcherHook<56>,catcherHook<57>,catcherHook<58>,catcherHook<59>,
        catcherHook<60>,catcherHook<61>,catcherHook<62>,catcherHook<63>
};

static void startServer() {
    httplib::Server svr;

    svr.Get("/",[](const httplib::Request&, httplib::Response& res) {
        int n = 0;
        for (auto& img : Image::GetImages()) if (img.IsValid()) n++;
        res.set_content(GetExplorerHTML(n), "text/html");
    });

    svr.Get("/api/assemblies",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(assembliesJson(), "application/json");
    });

    svr.Get("/api/classes",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(classesJson(req.get_param_value("a")), "application/json");
    });

    svr.Get("/api/allclasses",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(allClassesJson(), "application/json");
    });

    svr.Get("/api/class",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(classDetailJson(req.get_param_value("a"), req.get_param_value("ns"), req.get_param_value("n")), "application/json");
    });

    svr.Get("/api/instances",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(instancesJson(req.get_param_value("a"), req.get_param_value("ns"), req.get_param_value("n")), "application/json");
    });

    svr.Get("/api/scene",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(sceneJson(), "application/json");
    });

    svr.Get("/api/scene/inspect",[](const httplib::Request& req, httplib::Response& res) {
        auto s = req.get_param_value("addr");
        res.set_content(s.empty() ? "{}" : goInfoJson((uintptr_t)strtoull(s.c_str(), nullptr, 16)), "application/json");
    });

    svr.Get("/api/controller/inspect",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(req.get_param_value("addr").c_str(), nullptr, 16);
        std::string a = req.get_param_value("asm"), ns = req.get_param_value("ns"), cn = req.get_param_value("cls");

        Class startCls(ns.c_str(), cn.c_str(), Image(a.c_str()));
        if (!startCls.IsValid()) { res.set_content("{}", "application/json"); return; }

        std::ostringstream j;
        j << "{\"addr\":\"" << std::hex << addr << "\",\"name\":\"" << jsEsc(cn) << "\",\"fields\":[";

        bool firstF = true;
        std::vector<std::string> seen;

        for (Class cur = startCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
            auto* cn_ = cur.GetClass()->name;
            if (!cn_) break;
            if (strcmp(cn_, "Object") == 0 && cur.GetClass()->namespaze && strcmp(cur.GetClass()->namespaze, "System") == 0) break;

            for (auto& f : cur.GetFields(false)) {
                try {
                    auto* fi = f.GetInfo();
                    if (!fi || !fi->type) continue;
                    bool fStatic = (fi->type->attrs & 0x10) != 0;
                    if (!fStatic && !addr) continue;
                    std::string fn = fi->name;
                    bool dup = false; for (auto& s : seen) if (s==fn) dup=true;
                    if (dup) continue; seen.push_back(fn);
                    std::string ft = typeName(fi->type);
                    bool ok = false;
                    std::string vs = readField(ft, cur, fStatic ? nullptr : (Il2CppObject*)addr, fn, nullptr, fStatic, ok);
                    if (ok) { if (!firstF) j << ","; firstF = false; j << "{\"name\":\"" << jsEsc(fn) << "\",\"type\":\"" << ft << "\",\"val\":" << vs << ",\"isProp\":false,\"canWrite\":true,\"static\":" << (fStatic?"true":"false") << "}"; }
                } catch(...) {}
            }

            for (auto& p : cur.GetProperties(false)) {
                try {
                    auto* pi = p._data;
                    if (!pi || !pi->get) continue;
                    bool pStatic = (pi->get->flags & 0x10) != 0;
                    if (!pStatic && !addr) continue;
                    std::string pn = pi->name;
                    bool dup = false; for (auto& s : seen) if (s==pn) dup=true;
                    if (dup) continue; seen.push_back(pn);
                    std::string pt = typeName(pi->get->return_type);
                    bool ok = false;
                    MethodBase getter(pi->get);
                    std::string vs = readField(pt, cur, pStatic ? nullptr : (Il2CppObject*)addr, pn, &getter, pStatic, ok);
                    if (ok) { if (!firstF) j << ","; firstF = false; j << "{\"name\":\"" << jsEsc(pn) << "\",\"type\":\"" << pt << "\",\"val\":" << vs << ",\"isProp\":true,\"canWrite\":" << (pi->set?"true":"false") << ",\"static\":" << (pStatic?"true":"false") << "}"; }
                } catch(...) {}
            }
        }

        j << "],\"methods\":[";
        bool firstM = true;
        for (Class cur = startCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
            auto* cn_ = cur.GetClass()->name;
            if (!cn_) break;
            if (strcmp(cn_, "Object") == 0 && cur.GetClass()->namespaze && strcmp(cur.GetClass()->namespaze, "System") == 0) break;
            for (auto& m : cur.GetMethods(false)) {
                auto* mi = m.GetInfo();
                if (!m.IsValid() || !mi || !mi->name) continue;
                if (!firstM) j << ","; firstM = false;
                j << "{\"name\":\"" << jsEsc(mi->name) << "\",\"ret\":\"" << jsEsc(typeName(mi->return_type)) << "\",\"s\":" << ((mi->flags&0x10)?"true":"false") << ",\"params\":[";
                for (int i = 0; i < (int)mi->parameters_count; i++) {
                    if (i>0) j<<",";
                    j << "{\"n\":\"arg" << i << "\",\"t\":\"" << jsEsc(mi->parameters&&mi->parameters[i]?typeName(mi->parameters[i]):"?") << "\"}";
                }
                j << "]}";
            }
        }
        j << "]}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/scene/update",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        if (!addr) { res.set_content("{\"ok\":false}", "application/json"); return; }
        std::string type = jsonVal(req.body, "type"), prop = jsonVal(req.body, "prop"), val = jsonVal(req.body, "val");
        try {
            if (type == "gameobject") {
                if (prop == "active") {
                    bool newActive = val == "true";
                    Il2CppObject* go = resolveComponentByAddr(addr);
                    if (go) {
                        Method<void> setActive(Class("UnityEngine","GameObject").GetMethod("SetActive", 1));
                        if (setActive.IsValid()) setActive[go](newActive);
                    }
                } else if (prop == "name") {
                    Il2CppObject* go = resolveComponentByAddr(addr);
                    if (go) {
                        Method<void> m(Class("UnityEngine","Object").GetMethod("set_name",1));
                        if(m.IsValid()) m[go](CreateMonoString(val));
                    }
                }
            } else if (type == "transform") {
                float x=0,y=0,z=0; sscanf(val.c_str(),"[%f,%f,%f]",&x,&y,&z); Vector3 v={x,y,z};
                Il2CppObject* tr = resolveComponentByAddr(addr);
                if (tr) {
                    if (prop=="p") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localPosition",1)); if(m.IsValid()) m[tr](v); }
                    else if (prop=="r") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localEulerAngles",1)); if(m.IsValid()) m[tr](v); }
                    else if (prop=="s") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localScale",1)); if(m.IsValid()) m[tr](v); }
                }
            } else if (type == "script") {
                std::string name = jsonVal(req.body, "prop2");
                std::string ft   = jsonVal(req.body, "ftype");
                bool isProp      = jsonVal(req.body, "isProp") == "true";
                if (prop == "enabled") {
                    try {
                        bool newEnabled = val == "true";
                        Il2CppObject* comp = resolveComponentByAddr(addr);
                        if (comp && comp->klass) {
                            auto setEnabledOnComp = [&](Il2CppObject* c, bool v) {
                                Class compKlass(c->klass);
                                for (Class cur = compKlass; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                                    auto* cn_ = cur.GetClass()->name;
                                    if (!cn_ || strcmp(cn_, "Component") == 0 || strcmp(cn_, "Object") == 0) break;
                                    auto ep = cur.GetProperty("enabled");
                                    if (ep.IsValid() && ep._data && ep._data->set) {
                                        Method<void> m(ep._data->set); m.SetInstance(c); m(v); return true;
                                    }
                                }
                                return false;
                            };
                            auto getEnabledFromComp = [&](Il2CppObject* c) -> bool {
                                Class compKlass(c->klass);
                                for (Class cur = compKlass; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                                    auto* cn_ = cur.GetClass()->name;
                                    if (!cn_ || strcmp(cn_, "Component") == 0 || strcmp(cn_, "Object") == 0) break;
                                    auto ep = cur.GetProperty("enabled");
                                    if (ep.IsValid() && ep._data && ep._data->get) {
                                        Method<bool> m(ep._data->get); m.SetInstance(c);
                                        try { return m(); } catch(...) {}
                                    }
                                }
                                return false;
                            };
                            if (newEnabled) {
                                bool already = getEnabledFromComp(comp);
                                if (!already) {
                                    setEnabledOnComp(comp, true);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                    comp = resolveComponentByAddr(addr);
                                    if (!comp) { res.set_content("{\"ok\":true,\"warn\":\"enabled but object moved\"}", "application/json"); return; }
                                }
                            } else {
                                setEnabledOnComp(comp, false);
                            }
                        }
                    } catch(...) {}
                } else {
                    std::string compNs  = jsonVal(req.body, "compNs");
                    std::string compCls = jsonVal(req.body, "compCls");
                    std::string compAsm = jsonVal(req.body, "compAsm");
                    Il2CppObject* obj = resolveComponentByAddr(addr);
                    if (!obj) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
                    Class startCls = (obj->klass) ? Class(obj->klass) :
                                     Class(compNs.c_str(), compCls.c_str(), compAsm.empty() ? Image() : Image(compAsm.c_str()));
                    for (Class cur = startCls; cur.IsValid(); cur = cur.GetParent()) {
                        if (isProp) {
                            auto p = cur.GetProperty(name.c_str());
                            if (p.IsValid() && p._data && p._data->set) { MethodBase s(p._data->set); writeField(ft, cur, obj, name, &s, val); break; }
                        } else {
                            auto f = cur.GetField(name.c_str());
                            if (f.IsValid()) { writeField(ft, cur, obj, name, nullptr, val); break; }
                        }
                    }
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/instance/update",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        std::string a=jsonVal(req.body,"asm"), ns=jsonVal(req.body,"ns"), cn=jsonVal(req.body,"cls");
        std::string name=jsonVal(req.body,"name"), ft=jsonVal(req.body,"ftype"), val=jsonVal(req.body,"val");
        bool isProp = jsonVal(req.body,"isProp") == "true";
        bool isStaticField = jsonVal(req.body,"isStatic") == "true";
        Il2CppObject* obj = isStaticField ? nullptr : resolveComponentByAddr(addr);
        if (obj || isStaticField) {
            try {
                for (Class cur(ns.c_str(),cn.c_str(),Image(a.c_str())); cur.IsValid(); cur = cur.GetParent()) {
                    if (isProp) {
                        auto p = cur.GetProperty(name.c_str());
                        if (p.IsValid() && p._data && p._data->set) { MethodBase s(p._data->set); writeField(ft,cur,obj,name,&s,val,isStaticField); break; }
                    } else {
                        auto f = cur.GetField(name.c_str());
                        if (f.IsValid()) { writeField(ft,cur,obj,name,nullptr,val,isStaticField); break; }
                    }
                }
            } catch(...) {}
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/delete",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        auto s = jsonVal(req.body, "addr");
        if (!s.empty()) {
            uintptr_t addr = (uintptr_t)strtoull(s.c_str(), nullptr, 16);
            Il2CppObject* obj = resolveComponentByAddr(addr);
            if (obj) {
                try { Method<void> m(Class("UnityEngine","Object").GetMethod("Destroy",1)); if(m.IsValid()) m(obj); } catch(...) {}
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/invoke",[](const httplib::Request& req, httplib::Response& res) {
        auto r = invokeMethod(req.body);
        std::ostringstream j;
        j << "{\"ok\":" << (r.ok?"true":"false") << ",\"value\":\"" << jsEsc(r.val) << "\",\"error\":\"" << jsEsc(r.err) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/loop/add",[](const httplib::Request& req, httplib::Response& res) {
        auto e = std::make_shared<LoopEntry>();
        e->addr      = jsonVal(req.body, "addr");
        e->asm_      = jsonVal(req.body, "asm");
        e->ns        = jsonVal(req.body, "ns");
        e->cls       = jsonVal(req.body, "cls");
        e->name      = jsonVal(req.body, "name");
        e->ftype     = jsonVal(req.body, "ftype");
        e->isProp    = jsonVal(req.body, "isProp") == "true";
        e->val       = jsonVal(req.body, "val");
        int ms       = 0;
        try { ms = std::stoi(jsonVal(req.body, "interval")); } catch(...) {}
        if (ms < 50) ms = 100;
        e->intervalMs = ms;

        std::string id = e->addr + "_" + e->cls + "_" + e->name;
        {
            std::lock_guard<std::mutex> lk(g_loopMtx);
            auto it = g_loops.find(id);
            if (it != g_loops.end()) it->second->active = false;
            g_loops[id] = e;
        }

        std::thread([e, id]() {
            AttachThread();
            while (e->active) {
                applyLoopEntry(e);
                std::this_thread::sleep_for(std::chrono::milliseconds(e->intervalMs));
            }
            std::lock_guard<std::mutex> lk(g_loopMtx);
            auto it = g_loops.find(id);
            if (it != g_loops.end() && it->second == e) g_loops.erase(it);
        }).detach();

        std::ostringstream j;
        j << "{\"ok\":true,\"id\":\"" << jsEsc(id) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/loop/remove",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_loopMtx);
        auto it = g_loops.find(id);
        if (it != g_loops.end()) { it->second->active = false; g_loops.erase(it); }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/loop/removeall",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_loopMtx);
        for (auto& kv : g_loops) kv.second->active = false;
        g_loops.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/loop/list",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(loopListJson(), "application/json");
    });

    svr.Get("/api/logs",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(logsJson(), "application/json");
    });

    svr.Post("/api/logs/clear",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logBuffer.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/logs/add",[](const httplib::Request& req, httplib::Response& res) {
        std::string msg = jsonVal(req.body, "msg");
        int level = 0;
        try { level = std::stoi(jsonVal(req.body, "level")); } catch(...) {}
        addLogEntry(msg, level);
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/timescale",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        float ts = 1.0f;
        try {
            Class timeCls("UnityEngine", "Time");
            if (timeCls.IsValid()) {
                Method<float> get(timeCls.GetMethod("get_timeScale"));
                if (get.IsValid()) ts = get();
            }
        } catch(...) {}
        res.set_content("{\"value\":" + std::to_string(ts) + "}", "application/json");
    });

    svr.Post("/api/timescale",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        float val = 1.0f;
        try { val = std::stof(jsonVal(req.body, "value")); } catch(...) {}
        try {
            Class timeCls("UnityEngine", "Time");
            if (timeCls.IsValid()) {
                Method<void> set(timeCls.GetMethod("set_timeScale", 1));
                if (set.IsValid()) set(val);
            }
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/destroy",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        auto s = jsonVal(req.body, "addr");
        if (!s.empty()) {
            uintptr_t addr = (uintptr_t)strtoull(s.c_str(), nullptr, 16);
            Il2CppObject* obj = resolveComponentByAddr(addr);
            if (obj) {
                try {
                    Method<void> m(Class("UnityEngine","Object").GetMethod("Destroy",1));
                    if (m.IsValid()) m(obj);
                } catch(...) {}
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/create",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        std::string name = jsonVal(req.body, "name");
        if (name.empty()) name = "New GameObject";
        std::ostringstream j;
        try {
            Class goCls("UnityEngine", "GameObject");
            Method<Il2CppObject*> ctor;
            for (auto& m : goCls.GetMethods(false)) {
                auto* mi = m.GetInfo();
                if (mi && mi->name && strcmp(mi->name, ".ctor") == 0 && mi->parameters_count == 1) {
                    ctor = Method<Il2CppObject*>(m);
                    break;
                }
            }
            if (ctor.IsValid()) {
                Il2CppObject* newGO = goCls.CreateNewObjectParameters(CreateMonoString(name.c_str()));
                if (newGO) {
                    j << "{\"ok\":true,\"addr\":\"" << std::hex << (uintptr_t)newGO << "\"}";
                    res.set_content(j.str(), "application/json");
                    return;
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":false,\"error\":\"Failed to create GameObject\"}", "application/json");
    });

    svr.Post("/api/scene/addcomponent",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        std::string typeName_ = jsonVal(req.body, "type");
        if (!addr || typeName_.empty()) { res.set_content("{\"ok\":false}", "application/json"); return; }
        Il2CppObject* go = resolveComponentByAddr(addr);
        if (!go) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
        try {
            Il2CppObject* typeObj = sysTypeOf(typeName_);
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", UnityEngine.CoreModule");
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", UnityEngine");
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", Assembly-CSharp");
            if (typeObj) {
                Class goCls("UnityEngine", "GameObject");
                Method<Il2CppObject*> addComp(goCls.GetMethod("AddComponent", 1));
                if (addComp.IsValid()) {
                    Il2CppObject* comp = addComp[go](typeObj);
                    if (comp) {
                        std::ostringstream j;
                        j << "{\"ok\":true,\"addr\":\"" << std::hex << (uintptr_t)comp << "\"}";
                        res.set_content(j.str(), "application/json");
                        return;
                    }
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":false,\"error\":\"Failed to add component\"}", "application/json");
    });

    svr.Post("/api/scene/removecomponent",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        if (!addr) { res.set_content("{\"ok\":false}", "application/json"); return; }
        Il2CppObject* obj = resolveComponentByAddr(addr);
        if (!obj) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
        try {
            Method<void> destroy(Class("UnityEngine","Object").GetMethod("Destroy",1));
            if (destroy.IsValid()) destroy(obj);
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/dump",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        std::ostringstream out;
        for (auto& img : Image::GetImages()) {
            auto* d = img.GetInfo();
            if (!img.IsValid() || !d || !d->name) continue;
            out << "// Assembly: " << d->name << "\n\n";
            for (auto& cls : img.GetClasses(false)) {
                auto* r = cls.GetClass();
                if (!cls.IsValid() || !r || !r->name) continue;
                std::string clsKw = "class";
                if (r->enumtype) clsKw = "enum";
                else if (r->byval_arg.valuetype) clsKw = "struct";
                else if (r->flags & 0x20) clsKw = "interface";
                if (r->namespaze && strlen(r->namespaze) > 0) {
                    out << "namespace " << r->namespaze << " {\n";
                }
                std::string indent = (r->namespaze && strlen(r->namespaze) > 0) ? "    " : "";
                std::string parent = "";
                if (r->parent && r->parent->name && strcmp(r->parent->name, "Object") != 0 && strcmp(r->parent->name, "ValueType") != 0)
                    parent = " : " + std::string(r->parent->namespaze && strlen(r->parent->namespaze) > 0 ? std::string(r->parent->namespaze) + "." : "") + r->parent->name;
                out << indent << "public " << clsKw << " " << r->name << parent << " {\n";
                for (auto& f : cls.GetFields(false)) {
                    auto* fi = f.GetInfo();
                    if (!fi || !fi->name) continue;
                    std::string tn = typeName(fi->type);
                    bool isStatic = fi->type && (fi->type->attrs & 0x10);
                    out << indent << "    // offset 0x" << std::hex << fi->offset << std::dec << "\n";
                    out << indent << "    public " << (isStatic ? "static " : "") << tn << " " << fi->name << ";\n";
                }
                for (auto& p : cls.GetProperties(false)) {
                    auto* pi = p._data;
                    if (!pi || !pi->name) continue;
                    std::string tn = pi->get && pi->get->return_type ? typeName(pi->get->return_type) : "?";
                    out << indent << "    public " << tn << " " << pi->name << " {";
                    if (pi->get) out << " get;";
                    if (pi->set) out << " set;";
                    out << " }\n";
                }
                for (auto& m : cls.GetMethods(false)) {
                    auto* mi = m.GetInfo();
                    if (!mi || !mi->name) continue;
                    char ab[32] = {};
                    if (mi->methodPointer) snprintf(ab, sizeof(ab), "0x%llX", (unsigned long long)(uintptr_t)mi->methodPointer);
                    bool isStatic = mi->flags & 0x10;
                    out << indent << "    // " << ab << "\n";
                    out << indent << "    public " << (isStatic ? "static " : "") << typeName(mi->return_type) << " " << mi->name << "(";
                    for (int i = 0; i < (int)mi->parameters_count; i++) {
                        if (i > 0) out << ", ";
                        out << (mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?") << " arg" << i;
                    }
                    out << ") {}\n";
                }
                out << indent << "}\n";
                if (r->namespaze && strlen(r->namespaze) > 0) out << "}\n";
                out << "\n";
            }
        }
        res.set_content(out.str(), "text/plain");
    });

    svr.Get("/api/scene/addable",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        std::ostringstream j;
        j << "{";
        bool firstCat = true;
        static const struct { const char* cat; const char* ns; const char* name; } known[] = {
                {"Physics","UnityEngine","Rigidbody"},{"Physics","UnityEngine","BoxCollider"},{"Physics","UnityEngine","SphereCollider"},
                {"Physics","UnityEngine","CapsuleCollider"},{"Physics","UnityEngine","MeshCollider"},{"Physics","UnityEngine","CharacterController"},
                {"Physics","UnityEngine","WheelCollider"},{"Physics","UnityEngine","Rigidbody2D"},{"Physics","UnityEngine","BoxCollider2D"},
                {"Physics","UnityEngine","CircleCollider2D"},{"Physics","UnityEngine","PolygonCollider2D"},{"Physics","UnityEngine","CapsuleCollider2D"},
                {"Rendering","UnityEngine","MeshRenderer"},{"Rendering","UnityEngine","MeshFilter"},{"Rendering","UnityEngine","SkinnedMeshRenderer"},
                {"Rendering","UnityEngine","SpriteRenderer"},{"Rendering","UnityEngine","LineRenderer"},{"Rendering","UnityEngine","TrailRenderer"},
                {"Rendering","UnityEngine","Camera"},{"Rendering","UnityEngine","Light"},
                {"Audio","UnityEngine","AudioSource"},{"Audio","UnityEngine","AudioListener"},{"Audio","UnityEngine","AudioReverbZone"},
                {"Animation","UnityEngine","Animator"},{"Animation","UnityEngine","Animation"},{"Animation","UnityEngine","ParticleSystem"},
                {"Navigation","UnityEngine","NavMeshAgent"},{"Navigation","UnityEngine","NavMeshObstacle"},
                {"UI","UnityEngine.UI","Text"},{"UI","UnityEngine.UI","Image"},{"UI","UnityEngine.UI","Button"},
                {"UI","UnityEngine.UI","Toggle"},{"UI","UnityEngine.UI","Slider"},{"UI","UnityEngine.UI","InputField"},
                {"UI","UnityEngine.UI","Canvas"},{"UI","UnityEngine","CanvasGroup"},
                {nullptr,nullptr,nullptr}
        };
        std::map<std::string, std::vector<std::pair<std::string,std::string>>> cats;
        for (int i = 0; known[i].cat; i++) {
            Class c(known[i].ns, known[i].name);
            if (c.IsValid()) cats[known[i].cat].push_back({std::string(known[i].ns) + "." + known[i].name, known[i].name});
        }
        for (auto& img : Image::GetImages()) {
            auto* d = img.GetInfo();
            if (!img.IsValid() || !d || !d->name) continue;
            if (strstr(d->name, "Assembly-CSharp") == nullptr && strstr(d->name, "Assembly-CSharp-firstpass") == nullptr) continue;
            for (auto& cls : img.GetClasses(false)) {
                auto* r = cls.GetClass();
                if (!cls.IsValid() || !r || !r->name) continue;
                bool isMono = false;
                for (auto* cur = r; cur; cur = cur->parent)
                    if (cur->name && strcmp(cur->name, "MonoBehaviour") == 0) { isMono = true; break; }
                if (!isMono) continue;
                std::string ns = r->namespaze ? r->namespaze : "";
                std::string fullName = ns.empty() ? r->name : ns + "." + r->name;
                cats["Scripts"].push_back({fullName, r->name});
            }
        }
        for (auto& kv : cats) {
            if (!firstCat) j << ","; firstCat = false;
            j << "\"" << jsEsc(kv.first) << "\":[";
            bool first = true;
            for (auto& p : kv.second) {
                if (!first) j << ","; first = false;
                j << "{\"full\":\"" << jsEsc(p.first) << "\",\"name\":\"" << jsEsc(p.second) << "\"}";
            }
            j << "]";
        }
        j << "}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/catcher/hook",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        std::string asm_ = jsonVal(req.body, "asm");
        std::string ns   = jsonVal(req.body, "ns");
        std::string cls  = jsonVal(req.body, "cls");
        std::string meth = jsonVal(req.body, "method");
        std::string id   = asm_ + "|" + ns + "|" + cls + "|" + meth;
        {
            std::lock_guard<std::mutex> lk(g_catcherMtx);
            if (g_catchers.count(id)) {
                res.set_content(std::string("{\"ok\":true,\"exists\":true,\"id\":\"") + jsEsc(id) + "\"}", "application/json");
                return;
            }
        }
        int slot = -1;
        {
            std::lock_guard<std::mutex> lk(g_catcherSlotMtx);
            for (int i = 0; i < MAX_CATCHERS; i++) {
                if (!g_catcherSlots[i]) { slot = i; break; }
            }
        }
        if (slot < 0) { res.set_content("{\"ok\":false,\"error\":\"max catchers reached\"}", "application/json"); return; }
        auto entry = std::make_shared<CatcherEntry>();
        entry->id = id; entry->asm_ = asm_; entry->ns = ns; entry->cls = cls; entry->method = meth;
        entry->slotIdx = slot;
        Class c(ns.c_str(), cls.c_str(), Image(asm_.c_str()));
        if (!c.IsValid()) { res.set_content("{\"ok\":false,\"error\":\"class not found\"}", "application/json"); return; }
        MethodBase mb = c.GetMethod(meth.c_str());
        if (!mb.IsValid()) { res.set_content("{\"ok\":false,\"error\":\"method not found\"}", "application/json"); return; }
        auto* mi = mb.GetInfo();
        if (!mi || !mi->methodPointer) { res.set_content("{\"ok\":false,\"error\":\"no method pointer\"}", "application/json"); return; }
        entry->retType = typeName(mi->return_type);
        for (int i = 0; i < (int)mi->parameters_count; i++) {
            entry->paramTypes.push_back(mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?");
            entry->paramNames.push_back("arg" + std::to_string(i));
        }
        {
            std::lock_guard<std::mutex> lk(g_catcherSlotMtx);
            g_catcherSlots[slot] = new std::shared_ptr<CatcherEntry>(entry);
        }
        InvokeHook(mb, g_catcherHookFns[slot], g_catcherOrigPtrs[slot]);
        {
            std::lock_guard<std::mutex> lk(g_catcherMtx);
            g_catchers[id] = entry;
        }
        std::ostringstream j;
        j << "{\"ok\":true,\"id\":\"" << jsEsc(id) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/catcher/unhook",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it != g_catchers.end()) {
            it->second->active = false;
            int sl = it->second->slotIdx;
            if (sl >= 0 && sl < MAX_CATCHERS) {
                std::lock_guard<std::mutex> lk2(g_catcherSlotMtx);
                delete g_catcherSlots[sl];
                g_catcherSlots[sl] = nullptr;
                g_catcherOrigPtrs[sl] = nullptr;
            }
            g_catchers.erase(it);
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/catcher/clear",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it != g_catchers.end()) { std::lock_guard<std::mutex> lk2(it->second->callsMtx); it->second->calls.clear(); }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/catcher/list",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& kv : g_catchers) {
            auto& e = kv.second;
            if (!first) j << ","; first = false;
            j << "{\"id\":\"" << jsEsc(e->id) << "\",\"cls\":\"" << jsEsc(e->cls) << "\",\"ns\":\"" << jsEsc(e->ns) << "\",\"method\":\"" << jsEsc(e->method) << "\",\"ret\":\"" << jsEsc(e->retType) << "\",\"active\":" << (e->active?"true":"false") << ",\"count\":";
            { std::lock_guard<std::mutex> lk2(e->callsMtx); j << e->calls.size(); }
            j << ",\"params\":[";
            for (size_t i = 0; i < e->paramTypes.size(); i++) {
                if (i>0) j<<",";
                j << "{\"t\":\"" << jsEsc(e->paramTypes[i]) << "\",\"n\":\"" << jsEsc(e->paramNames[i]) << "\"}";
            }
            j << "]}";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    svr.Get("/api/catcher/calls",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it == g_catchers.end()) { res.set_content("[]", "application/json"); return; }
        auto& e = it->second;
        std::lock_guard<std::mutex> lk2(e->callsMtx);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& c : e->calls) {
            if (!first) j << ","; first = false;
            j << "{\"ts\":" << std::fixed << c.ts << ",\"instance\":\"" << jsEsc(c.instance) << "\",\"args\":[";
            for (size_t i = 0; i < c.args.size(); i++) {
                if (i>0) j<<",";
                j << "{\"t\":\"" << jsEsc(c.args[i].type) << "\",\"v\":" << c.args[i].val << "}";
            }
            j << "],\"ret\":\"" << jsEsc(c.ret) << "\"}";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/lua/exec",[](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string code = jsonVal(req.body, "code");
            if (code.empty()) { res.set_content("{\"output\":\"Error: empty code\"}", "application/json"); return; }
            std::string out = executeLuaBNM(code);
            std::ostringstream j;
            j << "{\"output\":\"" << jsEsc(out) << "\"}";
            res.set_content(j.str(), "application/json");
        } catch (...) {
            res.set_content("{\"output\":\"Error: native crash in executor\"}", "application/json");
        }
    });

    // --- LUA FILE MANAGER ---
    svr.Get("/api/lua/list", [](const httplib::Request&, httplib::Response& res) {
        std::string dir = GetScriptDir();
        std::string json = "[";
        bool first = true;
        DIR* dp = opendir(dir.c_str());
        if (dp != nullptr) {
            struct dirent* entry;
            while ((entry = readdir(dp)) != nullptr) {
                std::string fname = entry->d_name;
                if (fname.length() > 4 && fname.substr(fname.length() - 4) == ".lua") {
                    if (!first) json += ",";
                    json += "\"" + jsEsc(fname) + "\"";
                    first = false;
                }
            }
            closedir(dp);
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    svr.Post("/api/lua/save", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = jsonVal(req.body, "name");
        std::string code = jsonVal(req.body, "code");
        if (name.empty()) { res.set_content("{\"ok\":false}", "application/json"); return; }
        std::ofstream out(GetScriptDir() + name);
        if (out.is_open()) { out << code; out.close(); res.set_content("{\"ok\":true}", "application/json"); }
        else res.set_content("{\"ok\":false}", "application/json");
    });

    svr.Post("/api/lua/load", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = jsonVal(req.body, "name");
        std::ifstream in(GetScriptDir() + name);
        if (in.is_open()) {
            std::stringstream buffer; buffer << in.rdbuf();
            std::string codeEsc = jsEsc(buffer.str());
            res.set_content("{\"code\":\"" + codeEsc + "\"}", "application/json");
        } else {
            res.set_content("{}", "application/json");
        }
    });

    // --- MEMORY SCANNER NATIV ---
    svr.Post("/api/scanner/search", [](const httplib::Request& req, httplib::Response& res) {
        std::string type = jsonVal(req.body, "type");
        std::string valueStr = jsonVal(req.body, "value");
        if (type.empty() || valueStr.empty()) { res.set_content("{\"ok\":false}", "application/json"); return; }

        std::vector<uint8_t> searchBytes;
        int alignment = 1;

        try {
            if (type == "int32") {
                int32_t val = std::stoi(valueStr);
                searchBytes.resize(sizeof(val));
                memcpy(searchBytes.data(), &val, sizeof(val));
                alignment = 4;
            } else if (type == "float") {
                float val = std::stof(valueStr);
                searchBytes.resize(sizeof(val));
                memcpy(searchBytes.data(), &val, sizeof(val));
                alignment = 4;
            } else if (type == "string") {
                searchBytes.assign(valueStr.begin(), valueStr.end());
            }
        } catch (...) { res.set_content("{\"ok\":false,\"error\":\"Invalid value format\"}", "application/json"); return; }

        std::ostringstream results;
        results << "[";
        size_t count = 0;
        const size_t MAX_RESULTS = 150;

        std::ifstream maps("/proc/self/maps");
        int memFd = open("/proc/self/mem", O_RDONLY);

                if (maps.is_open() && memFd >= 0 && !searchBytes.empty()) {
            std::string line;
            bool first = true;
            while (std::getline(maps, line) && count < MAX_RESULTS) {
                char perms[5] = {0};
                uintptr_t start = 0, end = 0;
                
                if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) != 3) continue;
                if (strcmp(perms, "rw-p") != 0) continue;
                if (line.find("/dev/") != std::string::npos || line.find(".apk") != std::string::npos) continue;

                size_t regionSize = end - start;
                if (regionSize == 0 || regionSize > 50 * 1024 * 1024) continue;

                std::vector<uint8_t> buffer(regionSize);
                
                // AICI ERA PROBLEMA: Folosim > 0 pentru a accepta fragmentele de RAM citite parțial
                ssize_t bytesRead = pread64(memFd, buffer.data(), regionSize, start);
                
                if (bytesRead > 0) {
                    size_t searchLen = searchBytes.size();
                    if ((size_t)bytesRead >= searchLen) {
                        for (size_t i = 0; i <= (size_t)bytesRead - searchLen; i += alignment) {
                            if (memcmp(buffer.data() + i, searchBytes.data(), searchLen) == 0) {
                                if (!first) results << ",";
                                char addrBuf[32]; snprintf(addrBuf, sizeof(addrBuf), "0x%llX", (unsigned long long)(start + i));
                                results << "{\"addr\":\"" << addrBuf << "\",\"val\":\"" << jsEsc(valueStr) << "\"}";
                                first = false; count++;
                                if (count >= MAX_RESULTS) break;
                            }
                        }
                    }
                }
            }
        }

        if (memFd >= 0) close(memFd);
        results << "]";

        std::ostringstream response;
        response << "{\"ok\":true,\"count\":" << count << ",\"results\":" << results.str() << "}";
        res.set_content(response.str(), "application/json");
    });


    svr.listen("0.0.0.0", g_port);
}

static void OnLoaded() {
    hookDebugLog();
    std::thread(startServer).detach();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, [[maybe_unused]] void* reserved) {
    JNIEnv* env;
    vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    BNM::Loading::AddOnLoadedEvent(OnLoaded);
    BNM::Loading::TryLoadByJNI(env);
    return JNI_VERSION_1_6;
}static std::string jsEsc(const char* s) {
    if (!s) return "";
    std::string o;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
        else                 o += *p;
    }
    return o;
}

static std::string jsEsc(const std::string& s) { return jsEsc(s.c_str()); }

static std::string typeName(const Il2CppType* t) {
    if (!t) return "?";
    Class c(t);
    if (!c.IsValid()) return "?";
    auto* r = c.GetClass();
    if (!r || !r->name) return "?";
    std::string n = r->name;
    if (r->namespaze && strlen(r->namespaze) > 0)
        n = std::string(r->namespaze) + "." + n;
    return n;
}

struct LoopEntry {
    std::string addr;
    std::string asm_;
    std::string ns;
    std::string cls;
    std::string name;
    std::string ftype;
    bool isProp;
    std::string val;
    int intervalMs;
    std::atomic<bool> active{true};
};

static std::mutex g_loopMtx;
static std::map<std::string, std::shared_ptr<LoopEntry>> g_loops;

struct LogEntry {
    std::string msg;
    int level;
    double timestamp;
};
static std::mutex g_logMtx;
static std::vector<LogEntry> g_logBuffer;
static const size_t LOG_MAX = 500;
static std::atomic<bool> g_logHooked{false};

static void addLogEntry(const std::string& msg, int level) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logBuffer.size() >= LOG_MAX) g_logBuffer.erase(g_logBuffer.begin());
    g_logBuffer.push_back({msg, level, nowSeconds()});
}

static void hookDebugLog() {
    if (g_logHooked.exchange(true)) return;
    try {
        Class debugCls("UnityEngine", "Debug");
        if (!debugCls.IsValid()) return;
    } catch(...) {}
}

struct CatcherArg {
    std::string type;
    std::string val;
};

struct CatcherCall {
    double ts;
    std::string instance;
    std::vector<CatcherArg> args;
    std::string ret;
};

struct CatcherEntry {
    std::string id;
    std::string asm_;
    std::string ns;
    std::string cls;
    std::string method;
    std::string retType;
    std::vector<std::string> paramTypes;
    std::vector<std::string> paramNames;
    std::atomic<bool> active{true};
    std::mutex callsMtx;
    std::vector<CatcherCall> calls;
    void* origPtr = nullptr;
    int slotIdx = -1;
    static const size_t MAX_CALLS = 100;
};

static std::mutex g_catcherMtx;
static std::map<std::string, std::shared_ptr<CatcherEntry>> g_catchers;

static std::string logsJson() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& e : g_logBuffer) {
        if (!first) j << ","; first = false;
        j << "{\"msg\":\"" << jsEsc(e.msg) << "\",\"level\":" << e.level << ",\"t\":" << std::fixed << e.timestamp << "}";
    }
    j << "]";
    return j.str();
}

static std::string jsonVal(const std::string& json, const std::string& key) {
    std::string qk = "\"" + key + "\":";
    auto pos = json.find(qk);
    if (pos == std::string::npos) return "";
    pos += qk.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos < json.length() && json[pos] == '"') {
        pos++;
        size_t end = pos;
        bool esc = false;
        while (end < json.length()) {
            if (json[end] == '\\' && !esc) esc = true;
            else if (json[end] == '"' && !esc) break;
            else esc = false;
            end++;
        }
        if (end >= json.length()) return "";
        std::string raw = json.substr(pos, end - pos);
        std::string out;
        for (size_t i = 0; i < raw.length(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.length()) {
                char n = raw[i+1];
                if      (n == '"')  { out += '"';  i++; }
                else if (n == '\\') { out += '\\'; i++; }
                else if (n == 'n')  { out += '\n'; i++; }
                else if (n == 'r')  { out += '\r'; i++; }
                else out += raw[i];
            } else out += raw[i];
        }
        return out;
    }
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) return "";
    std::string v = json.substr(pos, end - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    return v;
}

static MethodBase findObjsMethod() {
    static MethodBase m;
    if (m.IsValid()) return m;
    Class oc("UnityEngine", "Object");
    for (auto& mt : oc.GetMethods(false)) {
        auto* mi = mt.GetInfo();
        if (mi && mi->name && strcmp(mi->name, "FindObjectsOfType") == 0 && mi->parameters_count == 1)
            if (mi->parameters[0] && strstr(typeName(mi->parameters[0]).c_str(), "Type"))
            { m = mt; break; }
    }
    return m;
}

static Il2CppObject* sysTypeOf(const std::string& aqn) {
    Class tc("System", "Type", Image("mscorlib.dll"));
    if (!tc.IsValid()) return nullptr;
    Method<Il2CppObject*> gt(tc.GetMethod("GetType", 1));
    return gt.IsValid() ? gt(CreateMonoString(aqn.c_str())) : nullptr;
}

static bool isObjectAlive(Il2CppObject* obj) {
    if (!obj) return false;
    try {
        Method<bool> m(Class("UnityEngine", "Object").GetMethod("op_Implicit", 1));
        if (m.IsValid()) return m(obj);
        auto* k = obj->klass;
        if (!k || !k->name) return false;
        return true;
    } catch(...) { return false; }
}

static Il2CppObject* resolveComponentByAddr(uintptr_t addr) {
    if (!addr) return nullptr;
    Il2CppObject* obj = (Il2CppObject*)addr;
    if (!isObjectAlive(obj)) return nullptr;
    return obj;
}

static std::string assembliesJson() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& img : Image::GetImages()) {
        auto* d = img.GetInfo();
        if (!img.IsValid() || !d || !d->name) continue;
        if (!first) j << ",";
        first = false;
        j << "\"" << jsEsc(d->name) << "\"";
    }
    j << "]";
    return j.str();
}

static std::string classesJson(const std::string& asm_) {
    Image img(asm_);
    if (!img.IsValid()) return "[]";
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& cls : img.GetClasses(false)) {
        auto* r = cls.GetClass();
        if (!cls.IsValid() || !r || !r->name) continue;
        if (!first) j << ",";
        first = false;
        std::string t = "class";
        if (r->enumtype) t = "enum";
        else if (r->byval_arg.valuetype) t = "struct";
        else if (r->flags & 0x20) t = "interface";
        j << "{\"name\":\"" << jsEsc(r->name) << "\",\"ns\":\"" << jsEsc(r->namespaze ? r->namespaze : "") << "\",\"t\":\"" << t << "\"}";
    }
    j << "]";
    return j.str();
}

static std::string allClassesJson() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& img : Image::GetImages()) {
        auto* d = img.GetInfo();
        if (!img.IsValid() || !d || !d->name) continue;
        std::string a = d->name;
        for (auto& cls : img.GetClasses(false)) {
            auto* r = cls.GetClass();
            if (!cls.IsValid() || !r || !r->name) continue;
            if (!first) j << ",";
            first = false;
            std::string t = "class";
            if (r->enumtype) t = "enum";
            else if (r->byval_arg.valuetype) t = "struct";
            else if (r->flags & 0x20) t = "interface";
            j << "{\"name\":\"" << jsEsc(r->name) << "\",\"ns\":\"" << jsEsc(r->namespaze ? r->namespaze : "") << "\",\"t\":\"" << t << "\",\"a\":\"" << jsEsc(a) << "\"}";
        }
    }
    j << "]";
    return j.str();
}

static std::string classDetailJson(const std::string& a, const std::string& ns, const std::string& cn) {
    Class cls(ns.c_str(), cn.c_str(), Image(a.c_str()));
    if (!cls.IsValid()) return "{}";
    auto* r = cls.GetClass();
    if (!r) return "{}";

    std::ostringstream j;
    j << "{\"name\":\"" << jsEsc(cn) << "\",\"ns\":\"" << jsEsc(ns) << "\",\"asm\":\"" << jsEsc(a) << "\",";
    j << (r->parent && r->parent->name ? "\"parent\":\"" + jsEsc(r->parent->name) + "\"," : "\"parent\":null,");

    j << "\"fields\":[";
    bool first = true;
    for (auto& f : cls.GetFields(false)) {
        auto* fi = f.GetInfo();
        if (!f.IsValid() || !fi || !fi->name) continue;
        if (!first) j << ",";
        first = false;
        j << "{\"name\":\"" << jsEsc(fi->name) << "\",\"type\":\"" << jsEsc(typeName(fi->type)) << "\",\"s\":" << (fi->type && (fi->type->attrs & 0x10) ? "true" : "false") << ",\"off\":" << fi->offset << "}";
    }

    j << "],\"methods\":[";
    first = true;
    for (auto& m : cls.GetMethods(false)) {
        auto* mi = m.GetInfo();
        if (!m.IsValid() || !mi || !mi->name) continue;
        if (!first) j << ",";
        first = false;
        char ab[32] = {};
        if (mi->methodPointer) snprintf(ab, sizeof(ab), "%llX", (unsigned long long)(uintptr_t)mi->methodPointer);
        j << "{\"name\":\"" << jsEsc(mi->name) << "\",\"ret\":\"" << jsEsc(typeName(mi->return_type)) << "\",\"s\":" << ((mi->flags & 0x10) ? "true" : "false") << ",\"addr\":\"" << ab << "\",\"params\":[";
        for (int i = 0; i < (int)mi->parameters_count; i++) {
            if (i > 0) j << ",";
            j << "{\"n\":\"arg" << i << "\",\"t\":\"" << jsEsc(mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?") << "\"}";
        }
        j << "]}";
    }

    j << "],\"props\":[";
    first = true;
    for (auto& p : cls.GetProperties(false)) {
        auto* pi = p._data;
        if (!p.IsValid() || !pi || !pi->name) continue;
        if (!first) j << ",";
        first = false;
        j << "{\"name\":\"" << jsEsc(pi->name) << "\",\"type\":\"" << jsEsc(pi->get && pi->get->return_type ? typeName(pi->get->return_type) : "?") << "\",\"g\":" << (pi->get ? "true" : "false") << ",\"s\":" << (pi->set ? "true" : "false") << "}";
    }
    j << "]}";
    return j.str();
}

struct InvokeResult { bool ok; std::string val, err; };

static InvokeResult invokeMethod(const std::string& body) {
    AttachThread();
    InvokeResult res = {false, "", ""};
    std::string a = jsonVal(body, "asm"), ns = jsonVal(body, "ns"), cn = jsonVal(body, "cls"), mn = jsonVal(body, "method");
    bool isStatic = jsonVal(body, "static") == "true" || jsonVal(body, "static") == "1";
    uintptr_t instAddr = (uintptr_t)strtoull(jsonVal(body, "instance").c_str(), nullptr, 16);

    std::vector<std::string> argT, argV;
    auto ap = body.find("\"args\":[");
    if (ap != std::string::npos) {
        ap += 8;
        auto ae = body.find("]", ap);
        if (ae != std::string::npos) {
            std::string ab = body.substr(ap, ae - ap);
            size_t p = 0;
            while (p < ab.size()) {
                auto ob = ab.find("{", p), cb = ab.find("}", ob);
                if (ob == std::string::npos || cb == std::string::npos) break;
                std::string e = "{" + ab.substr(ob + 1, cb - ob - 1) + "}";
                argT.push_back(jsonVal(e, "t"));
                argV.push_back(jsonVal(e, "v"));
                p = cb + 1;
            }
        }
    }

    if (a.empty() || cn.empty() || mn.empty()) { res.err = "Missing asm/cls/method"; return res; }

    Class cls(ns.c_str(), cn.c_str(), Image(a.c_str()));
    if (!cls.IsValid()) { res.err = "Class not found"; return res; }

    MethodBase mb = cls.GetMethod(mn.c_str(), (int)argT.size());
    if (!mb.IsValid()) mb = cls.GetMethod(mn.c_str());
    if (!mb.IsValid()) {
        for (Class cur = cls.GetParent(); cur.IsValid() && cur.GetClass() && !mb.IsValid(); cur = cur.GetParent()) {
            mb = cur.GetMethod(mn.c_str(), (int)argT.size());
            if (!mb.IsValid()) mb = cur.GetMethod(mn.c_str());
        }
    }
    if (!mb.IsValid()) { res.err = "Method not found"; return res; }
    auto* mi = mb.GetInfo();
    if (!mi || !mi->methodPointer) { res.err = "No pointer"; return res; }

    std::vector<void*> runtimeArgs;
    std::vector<int32_t> vi;
    std::vector<int64_t> vi64;
    std::vector<float> vf;
    std::vector<double> vd;
    std::vector<uint8_t> vb;
    std::vector<int16_t> vs16;
    std::vector<Vector3> vv3;
    std::vector<Vector2> vv2;
    std::vector<Color> vc;
    std::vector<Vector4> vv4;

    for (size_t i = 0; i < argT.size(); i++) {
        auto& t = argT[i]; auto& v = argV[i];
        if (t=="System.Int32"||t=="Int32"||t=="int") {
            vi.push_back(std::stoi(v.empty()?"0":v)); runtimeArgs.push_back(&vi.back());
        } else if (t=="System.Int64"||t=="Int64"||t=="long") {
            vi64.push_back(std::stoll(v.empty()?"0":v)); runtimeArgs.push_back(&vi64.back());
        } else if (t=="System.Single"||t=="Single"||t=="float") {
            vf.push_back(std::stof(v.empty()?"0":v)); runtimeArgs.push_back(&vf.back());
        } else if (t=="System.Double"||t=="Double"||t=="double") {
            vd.push_back(std::stod(v.empty()?"0":v)); runtimeArgs.push_back(&vd.back());
        } else if (t=="System.Boolean"||t=="Boolean"||t=="bool") {
            vb.push_back((uint8_t)(v=="true"||v=="1"?1:0)); runtimeArgs.push_back(&vb.back());
        } else if (t=="System.Byte"||t=="Byte"||t=="byte") {
            vb.push_back((uint8_t)std::stoul(v.empty()?"0":v)); runtimeArgs.push_back(&vb.back());
        } else if (t=="System.Int16"||t=="Int16"||t=="short") {
            vs16.push_back((int16_t)std::stoi(v.empty()?"0":v)); runtimeArgs.push_back(&vs16.back());
        } else if (t=="UnityEngine.Vector3"||t=="Vector3") {
            float x=0,y=0,z=0; sscanf(v.c_str(),"[%f,%f,%f]",&x,&y,&z); vv3.push_back({x,y,z}); runtimeArgs.push_back(&vv3.back());
        } else if (t=="UnityEngine.Vector2"||t=="Vector2") {
            float x=0,y=0; sscanf(v.c_str(),"[%f,%f]",&x,&y); vv2.push_back({x,y}); runtimeArgs.push_back(&vv2.back());
        } else if (t=="UnityEngine.Color"||t=="Color") {
            float r=1,g=1,b=1,aa=1; sscanf(v.c_str(),"[%f,%f,%f,%f]",&r,&g,&b,&aa); vc.push_back({r,g,b,aa}); runtimeArgs.push_back(&vc.back());
        } else if (t=="UnityEngine.Vector4"||t=="Vector4"||t=="UnityEngine.Quaternion"||t=="Quaternion") {
            float x=0,y=0,z=0,w=0; sscanf(v.c_str(),"[%f,%f,%f,%f]",&x,&y,&z,&w); vv4.push_back({x,y,z,w}); runtimeArgs.push_back(&vv4.back());
        } else if (t=="System.String"||t=="String"||t=="string") {
            runtimeArgs.push_back(CreateMonoString(v));
        } else {
            runtimeArgs.push_back((void*)strtoull(v.empty()?"0":v.c_str(), nullptr, 16));
        }
    }

    void* inst = isStatic ? nullptr : (void*)instAddr;
    std::string rtn = typeName(mi->return_type);
    std::ostringstream out;

    Il2CppException* exc = nullptr;
    void* ret = BNM::Internal::il2cppMethods.il2cpp_runtime_invoke(mi, inst, runtimeArgs.empty() ? nullptr : runtimeArgs.data(), &exc);

    if (exc) {
        BNM::Structures::Mono::String* excMsg = nullptr;
        try {
            auto excCls = Class(((Il2CppObject*)exc)->klass);
            if (excCls.IsValid()) {
                auto msgMethod = excCls.GetMethod("get_Message", 0);
                if (msgMethod.IsValid()) {
                    auto* msgMi = msgMethod.GetInfo();
                    if (msgMi && msgMi->methodPointer) {
                        Il2CppException* inner = nullptr;
                        auto* msgRet = (Il2CppObject*)BNM::Internal::il2cppMethods.il2cpp_runtime_invoke(msgMi, exc, nullptr, &inner);
                        if (msgRet && !inner) excMsg = (BNM::Structures::Mono::String*)msgRet;
                    }
                }
            }
        } catch(...) {}
        res.err = excMsg ? excMsg->str() : "IL2CPP exception";
        return res;
    }

    if (rtn=="System.Void"||rtn=="Void"||rtn=="void"||rtn=="?") {
        out << "void";
    } else if (rtn=="System.Single"||rtn=="Single"||rtn=="float") {
        float v = ret ? *(float*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Double"||rtn=="Double"||rtn=="double") {
        double v = ret ? *(double*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Int32"||rtn=="Int32"||rtn=="int") {
        int v = ret ? *(int*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Int64"||rtn=="Int64"||rtn=="long") {
        int64_t v = ret ? *(int64_t*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Boolean"||rtn=="Boolean"||rtn=="bool") {
        bool v = ret ? *(bool*)((uint8_t*)ret + sizeof(Il2CppObject)) : false;
        out << (v?"true":"false");
    } else if (rtn=="System.Byte"||rtn=="Byte"||rtn=="byte") {
        uint8_t v = ret ? *(uint8_t*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << (int)v;
    } else if (rtn=="UnityEngine.Vector3"||rtn=="Vector3") {
        Vector3 v = ret ? *(Vector3*)((uint8_t*)ret + sizeof(Il2CppObject)) : Vector3{0,0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f, %.4f]",v.x,v.y,v.z); out<<buf;
    } else if (rtn=="UnityEngine.Vector2"||rtn=="Vector2") {
        Vector2 v = ret ? *(Vector2*)((uint8_t*)ret + sizeof(Il2CppObject)) : Vector2{0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f]",v.x,v.y); out<<buf;
    } else if (rtn=="UnityEngine.Color"||rtn=="Color") {
        Color v = ret ? *(Color*)((uint8_t*)ret + sizeof(Il2CppObject)) : Color{0,0,0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f, %.4f, %.4f]",v.r,v.g,v.b,v.a); out<<buf;
    } else if (rtn=="System.String"||rtn=="String"||rtn=="string") {
        auto* s = (BNM::Structures::Mono::String*)ret;
        out << (s ? s->str() : "null");
    } else {
        char buf[64]; snprintf(buf,sizeof(buf),"0x%llX",(unsigned long long)(uintptr_t)ret);
        out << rtn << " @ " << buf;
    }
    res.ok = true; res.val = out.str();
    return res;
}

static std::string instancesJson(const std::string& a, const std::string& ns, const std::string& cn) {
    AttachThread();
    std::string full = ns.empty() ? cn : ns + "." + cn;
    std::string aNoExt = a;
    auto dp = aNoExt.find(".dll");
    if (dp != std::string::npos) aNoExt = aNoExt.substr(0, dp);

    Il2CppObject* st = sysTypeOf(full + ", " + aNoExt);
    if (!st) st = sysTypeOf(full);
    if (!st) return "{\"error\":\"Class not found / System.Type missing\"}";

    auto fm = findObjsMethod();
    if (!fm.IsValid()) return "{\"error\":\"FindObjectsOfType missing\"}";

    std::ostringstream j;
    j << "{\"instances\":[";
    try {
        auto* arr = Method<Array<Il2CppObject*>*>(fm)(st);
        if (arr && arr->capacity > 0) {
            bool first = true;
            Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
            for (int i = 0; i < arr->capacity; i++) {
                Il2CppObject* obj = arr->m_Items[i];
                if (!obj) continue;
                if (!first) j << ",";
                first = false;
                std::string name = "obj";
                if (gn.IsValid()) { auto* s = gn[obj](); if (s) name = s->str(); }
                char addr[32]; snprintf(addr, sizeof(addr), "%llX", (unsigned long long)(uintptr_t)obj);
                j << "{\"addr\":\"" << addr << "\",\"name\":\"" << jsEsc(name) << "\"}";
            }
        }
    } catch(...) {}
    j << "]}";
    return j.str();
}

static std::string sceneJson() {
    AttachThread();
    auto fm = findObjsMethod();
    if (!fm.IsValid()) return "[]";

    Il2CppObject* st = sysTypeOf("UnityEngine.GameObject, UnityEngine.CoreModule");
    if (!st) st = sysTypeOf("UnityEngine.GameObject, UnityEngine");
    if (!st) st = sysTypeOf("UnityEngine.GameObject");
    if (!st) return "[]";

    Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
    Method<bool> ga(Class("UnityEngine","GameObject").GetMethod("get_activeSelf"));
    Method<Il2CppObject*> gt(Class("UnityEngine","GameObject").GetMethod("get_transform"));
    Method<Il2CppObject*> tp(Class("UnityEngine","Transform").GetMethod("get_parent"));
    Method<Il2CppObject*> cg(Class("UnityEngine","Component").GetMethod("get_gameObject"));

    std::ostringstream j;
    j << "[";
    try {
        auto* arr = Method<Array<Il2CppObject*>*>(fm)(st);
        if (arr && arr->capacity > 0) {
            bool first = true;
            for (int i = 0; i < arr->capacity; i++) {
                Il2CppObject* go = arr->m_Items[i];
                if (!go) continue;
                std::string name = "Unknown";
                bool active = false;
                if (gn.IsValid()) { auto* s = gn[go](); if (s) name = s->str(); }
                if (ga.IsValid()) active = ga[go]();
                uintptr_t par = 0;
                if (gt.IsValid() && tp.IsValid() && cg.IsValid()) {
                    Il2CppObject* tr = gt[go]();
                    if (tr) { Il2CppObject* pt = tp[tr](); if (pt) { Il2CppObject* pg = cg[pt](); if (pg) par = (uintptr_t)pg; } }
                }
                if (!first) j << ",";
                first = false;
                j << "{\"addr\":\"" << std::hex << (uintptr_t)go << "\",\"name\":\"" << jsEsc(name) << "\",\"active\":" << (active?"true":"false") << ",\"parent\":\"" << std::hex << par << "\"}";
            }
        }
    } catch(...) {}
    j << "]";
    return j.str();
}

static std::string readField(const std::string& ft, Class& cls, Il2CppObject* inst, const std::string& name, MethodBase* getter, bool isStatic, bool& ok) {
    ok = false;
    if (!inst && !isStatic) return "";
    try {
        if (ft=="System.Single"||ft=="Single"||ft=="float") {
            float v = 0;
            if (getter) { Method<float> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<float> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Double"||ft=="Double"||ft=="double") {
            double v = 0;
            if (getter) { Method<double> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<double> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int32"||ft=="Int32"||ft=="int") {
            int v = 0;
            if (getter) { Method<int> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt32"||ft=="UInt32"||ft=="uint") {
            uint32_t v = 0;
            if (getter) { Method<uint32_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint32_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int64"||ft=="Int64"||ft=="long") {
            int64_t v = 0;
            if (getter) { Method<int64_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt64"||ft=="UInt64"||ft=="ulong") {
            uint64_t v = 0;
            if (getter) { Method<uint64_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int16"||ft=="Int16"||ft=="short") {
            int16_t v = 0;
            if (getter) { Method<int16_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt16"||ft=="UInt16"||ft=="ushort") {
            uint16_t v = 0;
            if (getter) { Method<uint16_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Byte"||ft=="Byte"||ft=="byte") {
            uint8_t v = 0;
            if (getter) { Method<uint8_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.SByte"||ft=="SByte"||ft=="sbyte") {
            int8_t v = 0;
            if (getter) { Method<int8_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Boolean"||ft=="Boolean"||ft=="bool") {
            bool v = false;
            if (getter) { Method<bool> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<bool> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return v ? "true" : "false";
        } else if (ft=="System.String"||ft=="String"||ft=="string") {
            BNM::Structures::Mono::String* s = nullptr;
            if (getter) { Method<BNM::Structures::Mono::String*> m(*getter); if(!isStatic) m.SetInstance(inst); s = m(); }
            else { Field<BNM::Structures::Mono::String*> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);s=f();} }
            ok = true; return s ? "\"" + jsEsc(s->str()) + "\"" : "\"\"";
        } else if (ft=="UnityEngine.Vector3"||ft=="Vector3") {
            Vector3 v = {0,0,0};
            if (getter) { Method<Vector3> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector3> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return "[" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "]";
        } else if (ft=="UnityEngine.Color"||ft=="Color") {
            Color c = {0,0,0,0};
            if (getter) { Method<Color> m(*getter); if(!isStatic) m.SetInstance(inst); c = m(); }
            else { Field<Color> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);c=f();} }
            ok = true; return "[" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a) + "]";
        } else if (ft=="UnityEngine.Vector2"||ft=="Vector2") {
            Vector2 v = {0,0};
            if (getter) { Method<Vector2> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector2> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return "[" + std::to_string(v.x) + "," + std::to_string(v.y) + "]";
        } else if (ft=="UnityEngine.Vector4"||ft=="Vector4"||ft=="UnityEngine.Quaternion"||ft=="Quaternion") {
            Vector4 v = {0,0,0,0};
            if (getter) { Method<Vector4> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector4> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::string("{\"x\":") + std::to_string(v.x) + ",\"y\":" + std::to_string(v.y) + ",\"z\":" + std::to_string(v.z) + ",\"w\":" + std::to_string(v.w) + "}";
        } else if (ft=="UnityEngine.LayerMask"||ft=="LayerMask") {
            int v = 0;
            if (getter) { Method<int> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="UnityEngine.Rect"||ft=="Rect") {
            Rect v = {0,0,0,0};
            if (getter) { Method<Rect> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Rect> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true;
            return std::string("{\"x\":") + std::to_string(v.x) + ",\"y\":" + std::to_string(v.y) + ",\"w\":" + std::to_string(v.w) + ",\"h\":" + std::to_string(v.h) + "}";
        }
    } catch(...) {}
    return "";
}

static void writeField(const std::string& ft, Class& cls, Il2CppObject* inst, const std::string& name, MethodBase* setter, const std::string& v, bool isStatic = false) {
    if (!inst && !isStatic) return;
    try {
        if (ft=="System.Single"||ft=="Single"||ft=="float") {
            float val = std::stof(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<float> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Double"||ft=="Double"||ft=="double") {
            double val = std::stod(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<double> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int32"||ft=="Int32"||ft=="int") {
            int val = std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt32"||ft=="UInt32"||ft=="uint") {
            uint32_t val = (uint32_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint32_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int64"||ft=="Int64"||ft=="long") {
            int64_t val = std::stoll(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt64"||ft=="UInt64"||ft=="ulong") {
            uint64_t val = std::stoull(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int16"||ft=="Int16"||ft=="short") {
            int16_t val = (int16_t)std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt16"||ft=="UInt16"||ft=="ushort") {
            uint16_t val = (uint16_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Byte"||ft=="Byte"||ft=="byte") {
            uint8_t val = (uint8_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.SByte"||ft=="SByte"||ft=="sbyte") {
            int8_t val = (int8_t)std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Boolean"||ft=="Boolean"||ft=="bool") {
            bool val = v=="true"||v=="1";
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<bool> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.String"||ft=="String"||ft=="string") {
            auto* s = CreateMonoString(v.c_str());
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(s); }
            else { Field<BNM::Structures::Mono::String*> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=s;} }
        } else if (ft=="UnityEngine.Vector3"||ft=="Vector3") {
            float x=0,y=0,z=0; sscanf(v.c_str(),"[%f,%f,%f]",&x,&y,&z); Vector3 val={x,y,z};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector3> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Color"||ft=="Color") {
            float r=1,g=1,b=1,a=1; sscanf(v.c_str(),"[%f,%f,%f,%f]",&r,&g,&b,&a); Color val={r,g,b,a};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Color> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Vector2"||ft=="Vector2") {
            float x=0,y=0; sscanf(v.c_str(),"[%f,%f]",&x,&y); Vector2 val={x,y};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector2> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Vector4"||ft=="Vector4"||ft=="UnityEngine.Quaternion"||ft=="Quaternion") {
            float x=0,y=0,z=0,w=0;
            sscanf(v.c_str(),"{\"x\":%f,\"y\":%f,\"z\":%f,\"w\":%f}",&x,&y,&z,&w);
            Vector4 val={x,y,z,w};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector4> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.LayerMask"||ft=="LayerMask") {
            int val = std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Rect"||ft=="Rect") {
            float x=0,y=0,w=0,h=0;
            sscanf(v.c_str(),"{\"x\":%f,\"y\":%f,\"w\":%f,\"h\":%f}",&x,&y,&w,&h);
            Rect val={x,y,w,h};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Rect> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        }
    } catch(...) {}
}

static std::string goInfoJson(uintptr_t addr) {
    AttachThread();
    if (!addr) return "{}";
    Il2CppObject* go = (Il2CppObject*)addr;
    if (!isObjectAlive(go)) return "{\"stale\":true}";
    std::string name = "Unknown";
    bool active = false;

    Class goCls("UnityEngine","GameObject");
    Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
    Method<bool> ga(goCls.GetMethod("get_activeSelf"));
    Method<Il2CppObject*> gt(goCls.GetMethod("get_transform"));

    try {
        if (gn.IsValid()) { auto* s=gn[go](); if(s) name=s->str(); }
        if (ga.IsValid()) active = ga[go]();
    } catch(...) {}

    std::ostringstream j;
    j << "{\"addr\":\"" << std::hex << addr << "\",\"name\":\"" << jsEsc(name) << "\",\"active\":" << (active?"true":"false") << ",\"transform\":{";
    try {
        if (gt.IsValid()) {
            Il2CppObject* tr = gt[go]();
            if (tr) {
                Method<Vector3> gp(Class("UnityEngine","Transform").GetMethod("get_localPosition"));
                Method<Vector3> gr(Class("UnityEngine","Transform").GetMethod("get_localEulerAngles"));
                Method<Vector3> gs(Class("UnityEngine","Transform").GetMethod("get_localScale"));
                Vector3 p=gp.IsValid()?gp[tr]():Vector3{0,0,0};
                Vector3 r=gr.IsValid()?gr[tr]():Vector3{0,0,0};
                Vector3 s=gs.IsValid()?gs[tr]():Vector3{0,0,0};
                j << "\"addr\":\"" << std::hex << (uintptr_t)tr << "\",\"p\":[" << p.x << "," << p.y << "," << p.z << "],\"r\":[" << r.x << "," << r.y << "," << r.z << "],\"s\":[" << s.x << "," << s.y << "," << s.z << "]";
            }
        }
    } catch(...) {}
    j << "},\"scripts\":[";

    try {
        Class componentCls("UnityEngine", "Component");
        Il2CppObject* compReflType = nullptr;
        if (componentCls.IsValid()) compReflType = (Il2CppObject*)componentCls.GetMonoType();
        if (!compReflType) compReflType = sysTypeOf("UnityEngine.Component, UnityEngine.CoreModule");
        if (!compReflType) compReflType = sysTypeOf("UnityEngine.Component");
        Array<Il2CppObject*>* comps = nullptr;
        if (!comps && compReflType) {
            for (auto& mt : goCls.GetMethods(true)) {
                auto* mi = mt.GetInfo();
                if (!mi || !mi->name || mi->is_generic) continue;
                if (strcmp(mi->name, "GetComponents") != 0) continue;
                if (mi->parameters_count == 1) {
                    try {
                        Method<Array<Il2CppObject*>*> m(mt);
                        comps = m[go](compReflType);
                    } catch(...) { LOGE("goInfoJson: GetComponents(Type) threw"); }
                    if (comps && comps->capacity > 0) break;
                    comps = nullptr;
                }
            }
        }
        if (!comps && compReflType) {
            for (auto& mt : goCls.GetMethods(true)) {
                auto* mi = mt.GetInfo();
                if (!mi || !mi->name) continue;
                if (strcmp(mi->name, "GetComponentsInternal") != 0) continue;
                LOGD("goInfoJson: found GetComponentsInternal with %d params", mi->parameters_count);
                if (mi->parameters_count >= 2) {
                    try {
                        if (mi->parameters_count == 6) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, true, false, true, false, nullptr);
                            LOGD("goInfoJson: GetComponentsInternal(6) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        } else if (mi->parameters_count == 5) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, true, false, true, false);
                            LOGD("goInfoJson: GetComponentsInternal(5) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        } else if (mi->parameters_count == 4) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, false, true, false);
                            LOGD("goInfoJson: GetComponentsInternal(4) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        }
                    } catch(...) { LOGE("goInfoJson: GetComponentsInternal threw"); }
                    if (comps && comps->capacity > 0) break;
                    comps = nullptr;
                }
            }
        }
        if (!comps) {
            LOGD("goInfoJson: falling back to FindObjectsOfType");
            Class monoCls("UnityEngine", "MonoBehaviour");
            Il2CppObject* monoType = nullptr;
            if (monoCls.IsValid()) monoType = (Il2CppObject*)monoCls.GetMonoType();
            if (!monoType) monoType = sysTypeOf("UnityEngine.MonoBehaviour, UnityEngine.CoreModule");
            if (!monoType) monoType = sysTypeOf("UnityEngine.MonoBehaviour");

            auto fom = findObjsMethod();
            Method<Il2CppObject*> compGetGO(Class("UnityEngine","Component").GetMethod("get_gameObject"));

            if (fom.IsValid() && monoType && compGetGO.IsValid()) {
                auto* allMonos = Method<Array<Il2CppObject*>*>(fom)(monoType);
                LOGD("goInfoJson: FindObjectsOfType(MonoBehaviour) count=%d", allMonos ? (int)allMonos->capacity : -1);
                if (allMonos && allMonos->capacity > 0) {
                    for (int i = 0; i < allMonos->capacity; i++) {
                        Il2CppObject* c = allMonos->m_Items[i];
                        if (!c) continue;
                        try {
                            Il2CppObject* cgo = compGetGO[c]();
                            if ((uintptr_t)cgo != addr) allMonos->m_Items[i] = nullptr;
                        } catch(...) { allMonos->m_Items[i] = nullptr; }
                    }
                    comps = allMonos;
                }
            }
        }

        LOGD("goInfoJson: final comps=%p, count=%d", comps, comps ? (int)comps->capacity : -1);

        static const char* kStopAtComponent[] = {
                "Component", "Object", nullptr
        };
        static const char* kSkipProps[] = {
                "isActiveAndEnabled", "transform", "gameObject", "tag", "name",
                "hideFlags", "rigidbody", "rigidbody2D", "camera", "light",
                "animation", "constantForce", "renderer", "audio", "networkView",
                "collider", "collider2D", "hingeJoint", "particleSystem", nullptr
        };

        auto isStopClass = [](const char* n) -> bool {
            for (auto** s = kStopAtComponent; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };
        auto isSkipProp = [](const char* n) -> bool {
            for (auto** s = kSkipProps; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };

        auto isKnownBuiltinBase = [](const char* n) -> bool {
            static const char* builtins[] = {
                    "MonoBehaviour", "Behaviour",
                    "Collider", "Collider2D",
                    "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "TerrainCollider", "WheelCollider",
                    "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "EdgeCollider2D", "CapsuleCollider2D", "CompositeCollider2D",
                    "Rigidbody", "Rigidbody2D",
                    "Renderer", "MeshRenderer", "SkinnedMeshRenderer", "SpriteRenderer", "LineRenderer", "TrailRenderer", "ParticleSystemRenderer",
                    "MeshFilter",
                    "Animator", "Animation",
                    "AudioSource", "AudioListener",
                    "Camera", "Light", "LensFlare", "Projector",
                    "Canvas", "CanvasGroup", "CanvasRenderer",
                    "RectTransform",
                    "Text", "Image", "RawImage", "Button", "Toggle", "Slider", "Scrollbar", "Dropdown", "InputField", "ScrollRect", "Mask", "RectMask2D",
                    "TMP_Text", "TextMeshPro", "TextMeshProUGUI",
                    "NavMeshAgent", "NavMeshObstacle",
                    "ParticleSystem",
                    "Joint", "HingeJoint", "FixedJoint", "SpringJoint", "CharacterJoint", "ConfigurableJoint",
                    "Joint2D", "HingeJoint2D", "FixedJoint2D", "SpringJoint2D", "WheelJoint2D", "SliderJoint2D",
                    "CharacterController",
                    "LODGroup",
                    "OcclusionArea", "OcclusionPortal",
                    "NetworkView",
                    "ConstantForce",
                    nullptr
            };
            for (auto** s = builtins; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };

        if (comps && comps->capacity > 0) {
            bool firstScript = true;

            for (int i = 0; i < comps->capacity; i++) {
                Il2CppObject* comp = comps->m_Items[i];
                if (!comp || !comp->klass) continue;

                auto* klass = comp->klass;

                bool isSupported = false;
                std::string compCategory = "script";
                for (auto* cur = klass; cur; cur = cur->parent) {
                    if (!cur->name) continue;
                    if (strcmp(cur->name, "MonoBehaviour") == 0) { isSupported = true; compCategory = "script"; break; }
                    if (isKnownBuiltinBase(cur->name) && strcmp(cur->name, "MonoBehaviour") != 0 && strcmp(cur->name, "Behaviour") != 0) {
                        isSupported = true; compCategory = "builtin"; break;
                    }
                    if (strcmp(cur->name, "Component") == 0 || strcmp(cur->name, "Object") == 0) break;
                }
                if (!isSupported) continue;

                std::string compName = klass->name ? klass->name : "Unknown";
                std::string compNs = klass->namespaze ? klass->namespaze : "";
                char compAddr[32]; snprintf(compAddr, sizeof(compAddr), "%llX", (unsigned long long)(uintptr_t)comp);
                std::string compAsm = (klass->image && klass->image->name) ? klass->image->name : "";

                bool hasEnabledProp = false;
                bool enabled = false;
                for (auto* cur = klass; cur; cur = cur->parent) {
                    if (!cur->name) continue;
                    if (strcmp(cur->name, "Behaviour") == 0 || strcmp(cur->name, "Renderer") == 0 ||
                        strcmp(cur->name, "Collider") == 0 || strcmp(cur->name, "Collider2D") == 0) {
                        hasEnabledProp = true; break;
                    }
                    if (strcmp(cur->name, "Component") == 0) break;
                }
                if (hasEnabledProp) {
                    Method<bool> getEnabled(Class("UnityEngine","Behaviour").GetMethod("get_enabled"));
                    if (!getEnabled.IsValid()) {
                        Class compClsCheck(klass);
                        auto ep = compClsCheck.GetProperty("enabled");
                        if (ep.IsValid() && ep._data && ep._data->get) {
                            MethodBase egetter(ep._data->get);
                            Method<bool> m(egetter); m.SetInstance(comp);
                            try { enabled = m(); } catch(...) {}
                        }
                    } else {
                        try { enabled = getEnabled[comp](); } catch(...) {}
                    }
                }

                if (!firstScript) j << ",";
                firstScript = false;
                j << "{\"addr\":\"" << compAddr << "\",\"name\":\"" << jsEsc(compName) << "\",\"ns\":\"" << jsEsc(compNs) << "\",\"asm\":\"" << jsEsc(compAsm) << "\",\"category\":\"" << compCategory << "\",\"enabled\":" << (enabled?"true":"false") << ",\"fields\":[";

                LOGD("goInfoJson: found component '%s' category='%s' at %s", compName.c_str(), compCategory.c_str(), compAddr);

                try {
                    Class compCls(klass);
                    bool firstF = true;
                    std::vector<std::string> seen;

                    for (Class cur = compCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                        auto* cn_ = cur.GetClass()->name;
                        if (!cn_) break;
                        if (isStopClass(cn_)) break;

                        for (auto& f : cur.GetFields(false)) {
                            try {
                                auto* fi = f.GetInfo();
                                if (!fi || !fi->type || (fi->type->attrs & 0x10)) continue;
                                std::string fn = fi->name;
                                bool dup = false; for (auto& s : seen) if (s==fn) dup=true;
                                if (dup) continue; seen.push_back(fn);
                                std::string ft = typeName(fi->type);
                                bool ok = false;
                                std::string vs = readField(ft, cur, comp, fn, nullptr, false, ok);
                                if (ok) {
                                    if (!firstF) j << ","; firstF = false;
                                    j << "{\"name\":\"" << jsEsc(fn) << "\",\"type\":\"" << jsEsc(ft) << "\",\"val\":" << vs << ",\"isProp\":false,\"canWrite\":true}";
                                }
                            } catch(...) {}
                        }

                        for (auto& p : cur.GetProperties(false)) {
                            try {
                                auto* pi = p._data;
                                if (!pi || !pi->get) continue;
                                std::string pn = pi->name;
                                if (isSkipProp(pn.c_str())) continue;
                                bool dup = false; for (auto& s : seen) if (s==pn) dup=true;
                                if (dup) continue; seen.push_back(pn);
                                std::string pt = typeName(pi->get->return_type);
                                bool ok = false;
                                MethodBase getter(pi->get);
                                std::string vs = readField(pt, cur, comp, pn, &getter, false, ok);
                                if (ok) {
                                    if (!firstF) j << ","; firstF = false;
                                    j << "{\"name\":\"" << jsEsc(pn) << "\",\"type\":\"" << jsEsc(pt) << "\",\"val\":" << vs << ",\"isProp\":true,\"canWrite\":" << (pi->set?"true":"false") << "}";
                                }
                            } catch(...) {}
                        }
                    }
                } catch(...) {}
                j << "]}";
            }
        }
    } catch(...) {
        LOGE("goInfoJson: exception in script enumeration");
    }
    j << "]}";
    return j.str();
}

static void applyLoopEntry(const std::shared_ptr<LoopEntry>& e) {
    try {
        uintptr_t addr = (uintptr_t)strtoull(e->addr.c_str(), nullptr, 16);
        if (!addr) return;
        Il2CppObject* obj = resolveComponentByAddr(addr);
        if (!obj) { e->active = false; return; }
        for (Class cur(e->ns.c_str(), e->cls.c_str(), Image(e->asm_.c_str())); cur.IsValid(); cur = cur.GetParent()) {
            if (e->isProp) {
                auto p = cur.GetProperty(e->name.c_str());
                if (p.IsValid() && p._data && p._data->set) {
                    MethodBase s(p._data->set);
                    writeField(e->ftype, cur, obj, e->name, &s, e->val);
                    return;
                }
            } else {
                auto f = cur.GetField(e->name.c_str());
                if (f.IsValid()) { writeField(e->ftype, cur, obj, e->name, nullptr, e->val); return; }
            }
        }
    } catch(...) {}
}

static std::string loopListJson() {
    std::lock_guard<std::mutex> lk(g_loopMtx);
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& kv : g_loops) {
        if (!first) j << ","; first = false;
        auto& e = kv.second;
        j << "{\"id\":\"" << jsEsc(kv.first) << "\",\"cls\":\"" << jsEsc(e->cls) << "\",\"name\":\"" << jsEsc(e->name) << "\",\"val\":\"" << jsEsc(e->val) << "\",\"interval\":" << e->intervalMs << ",\"active\":" << (e->active?"true":"false") << "}";
    }
    j << "]";
    return j.str();
}

#define MAX_CATCHERS 64
static std::shared_ptr<CatcherEntry>* g_catcherSlots[MAX_CATCHERS] = {};
static int g_catcherSlotCount = 0;
static std::mutex g_catcherSlotMtx;
static void (*g_catcherOrigPtrs[MAX_CATCHERS])(Il2CppObject*) = {};

template<int N>
static void catcherHook(Il2CppObject* self) {
    std::shared_ptr<CatcherEntry>* slot = g_catcherSlots[N];
    if (slot && *slot && (*slot)->active) {
        CatcherCall call;
        call.ts = nowSeconds();
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)(uintptr_t)self);
        call.instance = buf;
        if (self && self->klass) {
            try {
                Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
                if (gn.IsValid()) { auto* s = gn[self](); if (s) call.instance += " (" + s->str() + ")"; }
            } catch(...) {}
        }
        std::lock_guard<std::mutex> lk((*slot)->callsMtx);
        if ((*slot)->calls.size() >= CatcherEntry::MAX_CALLS) (*slot)->calls.erase((*slot)->calls.begin());
        (*slot)->calls.push_back(std::move(call));
    }
    if (g_catcherOrigPtrs[N]) g_catcherOrigPtrs[N](self);
}

typedef void(*CatcherHookFn)(Il2CppObject*);
static CatcherHookFn g_catcherHookFns[MAX_CATCHERS] = {
        catcherHook<0>,catcherHook<1>,catcherHook<2>,catcherHook<3>,catcherHook<4>,
        catcherHook<5>,catcherHook<6>,catcherHook<7>,catcherHook<8>,catcherHook<9>,
        catcherHook<10>,catcherHook<11>,catcherHook<12>,catcherHook<13>,catcherHook<14>,
        catcherHook<15>,catcherHook<16>,catcherHook<17>,catcherHook<18>,catcherHook<19>,
        catcherHook<20>,catcherHook<21>,catcherHook<22>,catcherHook<23>,catcherHook<24>,
        catcherHook<25>,catcherHook<26>,catcherHook<27>,catcherHook<28>,catcherHook<29>,
        catcherHook<30>,catcherHook<31>,catcherHook<32>,catcherHook<33>,catcherHook<34>,
        catcherHook<35>,catcherHook<36>,catcherHook<37>,catcherHook<38>,catcherHook<39>,
        catcherHook<40>,catcherHook<41>,catcherHook<42>,catcherHook<43>,catcherHook<44>,
        catcherHook<45>,catcherHook<46>,catcherHook<47>,catcherHook<48>,catcherHook<49>,
        catcherHook<50>,catcherHook<51>,catcherHook<52>,catcherHook<53>,catcherHook<54>,
        catcherHook<55>,catcherHook<56>,catcherHook<57>,catcherHook<58>,catcherHook<59>,
        catcherHook<60>,catcherHook<61>,catcherHook<62>,catcherHook<63>
};

static void startServer() {
    httplib::Server svr;

    svr.Get("/",[](const httplib::Request&, httplib::Response& res) {
        int n = 0;
        for (auto& img : Image::GetImages()) if (img.IsValid()) n++;
        res.set_content(GetExplorerHTML(n), "text/html");
    });

    svr.Get("/api/assemblies",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(assembliesJson(), "application/json");
    });

    svr.Get("/api/classes",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(classesJson(req.get_param_value("a")), "application/json");
    });

    svr.Get("/api/allclasses",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(allClassesJson(), "application/json");
    });

    svr.Get("/api/class",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(classDetailJson(req.get_param_value("a"), req.get_param_value("ns"), req.get_param_value("n")), "application/json");
    });

    svr.Get("/api/instances",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(instancesJson(req.get_param_value("a"), req.get_param_value("ns"), req.get_param_value("n")), "application/json");
    });

    svr.Get("/api/scene",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(sceneJson(), "application/json");
    });

    svr.Get("/api/scene/inspect",[](const httplib::Request& req, httplib::Response& res) {
        auto s = req.get_param_value("addr");
        res.set_content(s.empty() ? "{}" : goInfoJson((uintptr_t)strtoull(s.c_str(), nullptr, 16)), "application/json");
    });

    svr.Get("/api/controller/inspect",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(req.get_param_value("addr").c_str(), nullptr, 16);
        std::string a = req.get_param_value("asm"), ns = req.get_param_value("ns"), cn = req.get_param_value("cls");

        Class startCls(ns.c_str(), cn.c_str(), Image(a.c_str()));
        if (!startCls.IsValid()) { res.set_content("{}", "application/json"); return; }

        std::ostringstream j;
        j << "{\"addr\":\"" << std::hex << addr << "\",\"name\":\"" << jsEsc(cn) << "\",\"fields\":[";

        bool firstF = true;
        std::vector<std::string> seen;

        for (Class cur = startCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
            auto* cn_ = cur.GetClass()->name;
            if (!cn_) break;
            if (strcmp(cn_, "Object") == 0 && cur.GetClass()->namespaze && strcmp(cur.GetClass()->namespaze, "System") == 0) break;

            for (auto& f : cur.GetFields(false)) {
                try {
                    auto* fi = f.GetInfo();
                    if (!fi || !fi->type) continue;
                    bool fStatic = (fi->type->attrs & 0x10) != 0;
                    if (!fStatic && !addr) continue;
                    std::string fn = fi->name;
                    bool dup = false; for (auto& s : seen) if (s==fn) dup=true;
                    if (dup) continue; seen.push_back(fn);
                    std::string ft = typeName(fi->type);
                    bool ok = false;
                    std::string vs = readField(ft, cur, fStatic ? nullptr : (Il2CppObject*)addr, fn, nullptr, fStatic, ok);
                    if (ok) { if (!firstF) j << ","; firstF = false; j << "{\"name\":\"" << jsEsc(fn) << "\",\"type\":\"" << ft << "\",\"val\":" << vs << ",\"isProp\":false,\"canWrite\":true,\"static\":" << (fStatic?"true":"false") << "}"; }
                } catch(...) {}
            }

            for (auto& p : cur.GetProperties(false)) {
                try {
                    auto* pi = p._data;
                    if (!pi || !pi->get) continue;
                    bool pStatic = (pi->get->flags & 0x10) != 0;
                    if (!pStatic && !addr) continue;
                    std::string pn = pi->name;
                    bool dup = false; for (auto& s : seen) if (s==pn) dup=true;
                    if (dup) continue; seen.push_back(pn);
                    std::string pt = typeName(pi->get->return_type);
                    bool ok = false;
                    MethodBase getter(pi->get);
                    std::string vs = readField(pt, cur, pStatic ? nullptr : (Il2CppObject*)addr, pn, &getter, pStatic, ok);
                    if (ok) { if (!firstF) j << ","; firstF = false; j << "{\"name\":\"" << jsEsc(pn) << "\",\"type\":\"" << pt << "\",\"val\":" << vs << ",\"isProp\":true,\"canWrite\":" << (pi->set?"true":"false") << ",\"static\":" << (pStatic?"true":"false") << "}"; }
                } catch(...) {}
            }
        }

        j << "],\"methods\":[";
        bool firstM = true;
        for (Class cur = startCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
            auto* cn_ = cur.GetClass()->name;
            if (!cn_) break;
            if (strcmp(cn_, "Object") == 0 && cur.GetClass()->namespaze && strcmp(cur.GetClass()->namespaze, "System") == 0) break;
            for (auto& m : cur.GetMethods(false)) {
                auto* mi = m.GetInfo();
                if (!m.IsValid() || !mi || !mi->name) continue;
                if (!firstM) j << ","; firstM = false;
                j << "{\"name\":\"" << jsEsc(mi->name) << "\",\"ret\":\"" << jsEsc(typeName(mi->return_type)) << "\",\"s\":" << ((mi->flags&0x10)?"true":"false") << ",\"params\":[";
                for (int i = 0; i < (int)mi->parameters_count; i++) {
                    if (i>0) j<<",";
                    j << "{\"n\":\"arg" << i << "\",\"t\":\"" << jsEsc(mi->parameters&&mi->parameters[i]?typeName(mi->parameters[i]):"?") << "\"}";
                }
                j << "]}";
            }
        }
        j << "]}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/scene/update",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        if (!addr) { res.set_content("{\"ok\":false}", "application/json"); return; }
        std::string type = jsonVal(req.body, "type"), prop = jsonVal(req.body, "prop"), val = jsonVal(req.body, "val");
        try {
            if (type == "gameobject") {
                if (prop == "active") {
                    bool newActive = val == "true";
                    Il2CppObject* go = resolveComponentByAddr(addr);
                    if (go) {
                        Method<void> setActive(Class("UnityEngine","GameObject").GetMethod("SetActive", 1));
                        if (setActive.IsValid()) setActive[go](newActive);
                    }
                } else if (prop == "name") {
                    Il2CppObject* go = resolveComponentByAddr(addr);
                    if (go) {
                        Method<void> m(Class("UnityEngine","Object").GetMethod("set_name",1));
                        if(m.IsValid()) m[go](CreateMonoString(val));
                    }
                }
            } else if (type == "transform") {
                float x=0,y=0,z=0; sscanf(val.c_str(),"[%f,%f,%f]",&x,&y,&z); Vector3 v={x,y,z};
                Il2CppObject* tr = resolveComponentByAddr(addr);
                if (tr) {
                    if (prop=="p") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localPosition",1)); if(m.IsValid()) m[tr](v); }
                    else if (prop=="r") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localEulerAngles",1)); if(m.IsValid()) m[tr](v); }
                    else if (prop=="s") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localScale",1)); if(m.IsValid()) m[tr](v); }
                }
            } else if (type == "script") {
                std::string name = jsonVal(req.body, "prop2");
                std::string ft   = jsonVal(req.body, "ftype");
                bool isProp      = jsonVal(req.body, "isProp") == "true";
                if (prop == "enabled") {
                    try {
                        bool newEnabled = val == "true";
                        Il2CppObject* comp = resolveComponentByAddr(addr);
                        if (comp && comp->klass) {
                            auto setEnabledOnComp = [&](Il2CppObject* c, bool v) {
                                Class compKlass(c->klass);
                                for (Class cur = compKlass; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                                    auto* cn_ = cur.GetClass()->name;
                                    if (!cn_ || strcmp(cn_, "Component") == 0 || strcmp(cn_, "Object") == 0) break;
                                    auto ep = cur.GetProperty("enabled");
                                    if (ep.IsValid() && ep._data && ep._data->set) {
                                        Method<void> m(ep._data->set); m.SetInstance(c); m(v); return true;
                                    }
                                }
                                return false;
                            };
                            auto getEnabledFromComp = [&](Il2CppObject* c) -> bool {
                                Class compKlass(c->klass);
                                for (Class cur = compKlass; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                                    auto* cn_ = cur.GetClass()->name;
                                    if (!cn_ || strcmp(cn_, "Component") == 0 || strcmp(cn_, "Object") == 0) break;
                                    auto ep = cur.GetProperty("enabled");
                                    if (ep.IsValid() && ep._data && ep._data->get) {
                                        Method<bool> m(ep._data->get); m.SetInstance(c);
                                        try { return m(); } catch(...) {}
                                    }
                                }
                                return false;
                            };
                            if (newEnabled) {
                                bool already = getEnabledFromComp(comp);
                                if (!already) {
                                    setEnabledOnComp(comp, true);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                    comp = resolveComponentByAddr(addr);
                                    if (!comp) { res.set_content("{\"ok\":true,\"warn\":\"enabled but object moved\"}", "application/json"); return; }
                                }
                            } else {
                                setEnabledOnComp(comp, false);
                            }
                        }
                    } catch(...) {}
                } else {
                    std::string compNs  = jsonVal(req.body, "compNs");
                    std::string compCls = jsonVal(req.body, "compCls");
                    std::string compAsm = jsonVal(req.body, "compAsm");
                    Il2CppObject* obj = resolveComponentByAddr(addr);
                    if (!obj) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
                    Class startCls = (obj->klass) ? Class(obj->klass) :
                                     Class(compNs.c_str(), compCls.c_str(), compAsm.empty() ? Image() : Image(compAsm.c_str()));
                    for (Class cur = startCls; cur.IsValid(); cur = cur.GetParent()) {
                        if (isProp) {
                            auto p = cur.GetProperty(name.c_str());
                            if (p.IsValid() && p._data && p._data->set) { MethodBase s(p._data->set); writeField(ft, cur, obj, name, &s, val); break; }
                        } else {
                            auto f = cur.GetField(name.c_str());
                            if (f.IsValid()) { writeField(ft, cur, obj, name, nullptr, val); break; }
                        }
                    }
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/instance/update",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        std::string a=jsonVal(req.body,"asm"), ns=jsonVal(req.body,"ns"), cn=jsonVal(req.body,"cls");
        std::string name=jsonVal(req.body,"name"), ft=jsonVal(req.body,"ftype"), val=jsonVal(req.body,"val");
        bool isProp = jsonVal(req.body,"isProp") == "true";
        bool isStaticField = jsonVal(req.body,"isStatic") == "true";
        Il2CppObject* obj = isStaticField ? nullptr : resolveComponentByAddr(addr);
        if (obj || isStaticField) {
            try {
                for (Class cur(ns.c_str(),cn.c_str(),Image(a.c_str())); cur.IsValid(); cur = cur.GetParent()) {
                    if (isProp) {
                        auto p = cur.GetProperty(name.c_str());
                        if (p.IsValid() && p._data && p._data->set) { MethodBase s(p._data->set); writeField(ft,cur,obj,name,&s,val,isStaticField); break; }
                    } else {
                        auto f = cur.GetField(name.c_str());
                        if (f.IsValid()) { writeField(ft,cur,obj,name,nullptr,val,isStaticField); break; }
                    }
                }
            } catch(...) {}
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/delete",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        auto s = jsonVal(req.body, "addr");
        if (!s.empty()) {
            uintptr_t addr = (uintptr_t)strtoull(s.c_str(), nullptr, 16);
            Il2CppObject* obj = resolveComponentByAddr(addr);
            if (obj) {
                try { Method<void> m(Class("UnityEngine","Object").GetMethod("Destroy",1)); if(m.IsValid()) m(obj); } catch(...) {}
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/invoke",[](const httplib::Request& req, httplib::Response& res) {
        auto r = invokeMethod(req.body);
        std::ostringstream j;
        j << "{\"ok\":" << (r.ok?"true":"false") << ",\"value\":\"" << jsEsc(r.val) << "\",\"error\":\"" << jsEsc(r.err) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/loop/add",[](const httplib::Request& req, httplib::Response& res) {
        auto e = std::make_shared<LoopEntry>();
        e->addr      = jsonVal(req.body, "addr");
        e->asm_      = jsonVal(req.body, "asm");
        e->ns        = jsonVal(req.body, "ns");
        e->cls       = jsonVal(req.body, "cls");
        e->name      = jsonVal(req.body, "name");
        e->ftype     = jsonVal(req.body, "ftype");
        e->isProp    = jsonVal(req.body, "isProp") == "true";
        e->val       = jsonVal(req.body, "val");
        int ms       = 0;
        try { ms = std::stoi(jsonVal(req.body, "interval")); } catch(...) {}
        if (ms < 50) ms = 100;
        e->intervalMs = ms;

        std::string id = e->addr + "_" + e->cls + "_" + e->name;
        {
            std::lock_guard<std::mutex> lk(g_loopMtx);
            auto it = g_loops.find(id);
            if (it != g_loops.end()) it->second->active = false;
            g_loops[id] = e;
        }

        std::thread([e, id]() {
            AttachThread();
            while (e->active) {
                applyLoopEntry(e);
                std::this_thread::sleep_for(std::chrono::milliseconds(e->intervalMs));
            }
            std::lock_guard<std::mutex> lk(g_loopMtx);
            auto it = g_loops.find(id);
            if (it != g_loops.end() && it->second == e) g_loops.erase(it);
        }).detach();

        std::ostringstream j;
        j << "{\"ok\":true,\"id\":\"" << jsEsc(id) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/loop/remove",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_loopMtx);
        auto it = g_loops.find(id);
        if (it != g_loops.end()) { it->second->active = false; g_loops.erase(it); }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/loop/removeall",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_loopMtx);
        for (auto& kv : g_loops) kv.second->active = false;
        g_loops.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/loop/list",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(loopListJson(), "application/json");
    });

    svr.Get("/api/logs",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(logsJson(), "application/json");
    });

    svr.Post("/api/logs/clear",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logBuffer.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/logs/add",[](const httplib::Request& req, httplib::Response& res) {
        std::string msg = jsonVal(req.body, "msg");
        int level = 0;
        try { level = std::stoi(jsonVal(req.body, "level")); } catch(...) {}
        addLogEntry(msg, level);
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/timescale",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        float ts = 1.0f;
        try {
            Class timeCls("UnityEngine", "Time");
            if (timeCls.IsValid()) {
                Method<float> get(timeCls.GetMethod("get_timeScale"));
                if (get.IsValid()) ts = get();
            }
        } catch(...) {}
        res.set_content("{\"value\":" + std::to_string(ts) + "}", "application/json");
    });

    svr.Post("/api/timescale",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        float val = 1.0f;
        try { val = std::stof(jsonVal(req.body, "value")); } catch(...) {}
        try {
            Class timeCls("UnityEngine", "Time");
            if (timeCls.IsValid()) {
                Method<void> set(timeCls.GetMethod("set_timeScale", 1));
                if (set.IsValid()) set(val);
            }
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/destroy",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        auto s = jsonVal(req.body, "addr");
        if (!s.empty()) {
            uintptr_t addr = (uintptr_t)strtoull(s.c_str(), nullptr, 16);
            Il2CppObject* obj = resolveComponentByAddr(addr);
            if (obj) {
                try {
                    Method<void> m(Class("UnityEngine","Object").GetMethod("Destroy",1));
                    if (m.IsValid()) m(obj);
                } catch(...) {}
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/create",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        std::string name = jsonVal(req.body, "name");
        if (name.empty()) name = "New GameObject";
        std::ostringstream j;
        try {
            Class goCls("UnityEngine", "GameObject");
            Method<Il2CppObject*> ctor;
            for (auto& m : goCls.GetMethods(false)) {
                auto* mi = m.GetInfo();
                if (mi && mi->name && strcmp(mi->name, ".ctor") == 0 && mi->parameters_count == 1) {
                    ctor = Method<Il2CppObject*>(m);
                    break;
                }
            }
            if (ctor.IsValid()) {
                Il2CppObject* newGO = goCls.CreateNewObjectParameters(CreateMonoString(name.c_str()));
                if (newGO) {
                    j << "{\"ok\":true,\"addr\":\"" << std::hex << (uintptr_t)newGO << "\"}";
                    res.set_content(j.str(), "application/json");
                    return;
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":false,\"error\":\"Failed to create GameObject\"}", "application/json");
    });

    svr.Post("/api/scene/addcomponent",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        std::string typeName_ = jsonVal(req.body, "type");
        if (!addr || typeName_.empty()) { res.set_content("{\"ok\":false}", "application/json"); return; }
        Il2CppObject* go = resolveComponentByAddr(addr);
        if (!go) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
        try {
            Il2CppObject* typeObj = sysTypeOf(typeName_);
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", UnityEngine.CoreModule");
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", UnityEngine");
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", Assembly-CSharp");
            if (typeObj) {
                Class goCls("UnityEngine", "GameObject");
                Method<Il2CppObject*> addComp(goCls.GetMethod("AddComponent", 1));
                if (addComp.IsValid()) {
                    Il2CppObject* comp = addComp[go](typeObj);
                    if (comp) {
                        std::ostringstream j;
                        j << "{\"ok\":true,\"addr\":\"" << std::hex << (uintptr_t)comp << "\"}";
                        res.set_content(j.str(), "application/json");
                        return;
                    }
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":false,\"error\":\"Failed to add component\"}", "application/json");
    });

    svr.Post("/api/scene/removecomponent",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        if (!addr) { res.set_content("{\"ok\":false}", "application/json"); return; }
        Il2CppObject* obj = resolveComponentByAddr(addr);
        if (!obj) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
        try {
            Method<void> destroy(Class("UnityEngine","Object").GetMethod("Destroy",1));
            if (destroy.IsValid()) destroy(obj);
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/dump",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        std::ostringstream out;
        for (auto& img : Image::GetImages()) {
            auto* d = img.GetInfo();
            if (!img.IsValid() || !d || !d->name) continue;
            out << "// Assembly: " << d->name << "\n\n";
            for (auto& cls : img.GetClasses(false)) {
                auto* r = cls.GetClass();
                if (!cls.IsValid() || !r || !r->name) continue;
                std::string clsKw = "class";
                if (r->enumtype) clsKw = "enum";
                else if (r->byval_arg.valuetype) clsKw = "struct";
                else if (r->flags & 0x20) clsKw = "interface";
                if (r->namespaze && strlen(r->namespaze) > 0) {
                    out << "namespace " << r->namespaze << " {\n";
                }
                std::string indent = (r->namespaze && strlen(r->namespaze) > 0) ? "    " : "";
                std::string parent = "";
                if (r->parent && r->parent->name && strcmp(r->parent->name, "Object") != 0 && strcmp(r->parent->name, "ValueType") != 0)
                    parent = " : " + std::string(r->parent->namespaze && strlen(r->parent->namespaze) > 0 ? std::string(r->parent->namespaze) + "." : "") + r->parent->name;
                out << indent << "public " << clsKw << " " << r->name << parent << " {\n";
                for (auto& f : cls.GetFields(false)) {
                    auto* fi = f.GetInfo();
                    if (!fi || !fi->name) continue;
                    std::string tn = typeName(fi->type);
                    bool isStatic = fi->type && (fi->type->attrs & 0x10);
                    out << indent << "    // offset 0x" << std::hex << fi->offset << std::dec << "\n";
                    out << indent << "    public " << (isStatic ? "static " : "") << tn << " " << fi->name << ";\n";
                }
                for (auto& p : cls.GetProperties(false)) {
                    auto* pi = p._data;
                    if (!pi || !pi->name) continue;
                    std::string tn = pi->get && pi->get->return_type ? typeName(pi->get->return_type) : "?";
                    out << indent << "    public " << tn << " " << pi->name << " {";
                    if (pi->get) out << " get;";
                    if (pi->set) out << " set;";
                    out << " }\n";
                }
                for (auto& m : cls.GetMethods(false)) {
                    auto* mi = m.GetInfo();
                    if (!mi || !mi->name) continue;
                    char ab[32] = {};
                    if (mi->methodPointer) snprintf(ab, sizeof(ab), "0x%llX", (unsigned long long)(uintptr_t)mi->methodPointer);
                    bool isStatic = mi->flags & 0x10;
                    out << indent << "    // " << ab << "\n";
                    out << indent << "    public " << (isStatic ? "static " : "") << typeName(mi->return_type) << " " << mi->name << "(";
                    for (int i = 0; i < (int)mi->parameters_count; i++) {
                        if (i > 0) out << ", ";
                        out << (mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?") << " arg" << i;
                    }
                    out << ") {}\n";
                }
                out << indent << "}\n";
                if (r->namespaze && strlen(r->namespaze) > 0) out << "}\n";
                out << "\n";
            }
        }
        res.set_content(out.str(), "text/plain");
    });

    svr.Get("/api/scene/addable",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        std::ostringstream j;
        j << "{";
        bool firstCat = true;
        static const struct { const char* cat; const char* ns; const char* name; } known[] = {
                {"Physics","UnityEngine","Rigidbody"},{"Physics","UnityEngine","BoxCollider"},{"Physics","UnityEngine","SphereCollider"},
                {"Physics","UnityEngine","CapsuleCollider"},{"Physics","UnityEngine","MeshCollider"},{"Physics","UnityEngine","CharacterController"},
                {"Physics","UnityEngine","WheelCollider"},{"Physics","UnityEngine","Rigidbody2D"},{"Physics","UnityEngine","BoxCollider2D"},
                {"Physics","UnityEngine","CircleCollider2D"},{"Physics","UnityEngine","PolygonCollider2D"},{"Physics","UnityEngine","CapsuleCollider2D"},
                {"Rendering","UnityEngine","MeshRenderer"},{"Rendering","UnityEngine","MeshFilter"},{"Rendering","UnityEngine","SkinnedMeshRenderer"},
                {"Rendering","UnityEngine","SpriteRenderer"},{"Rendering","UnityEngine","LineRenderer"},{"Rendering","UnityEngine","TrailRenderer"},
                {"Rendering","UnityEngine","Camera"},{"Rendering","UnityEngine","Light"},
                {"Audio","UnityEngine","AudioSource"},{"Audio","UnityEngine","AudioListener"},{"Audio","UnityEngine","AudioReverbZone"},
                {"Animation","UnityEngine","Animator"},{"Animation","UnityEngine","Animation"},{"Animation","UnityEngine","ParticleSystem"},
                {"Navigation","UnityEngine","NavMeshAgent"},{"Navigation","UnityEngine","NavMeshObstacle"},
                {"UI","UnityEngine.UI","Text"},{"UI","UnityEngine.UI","Image"},{"UI","UnityEngine.UI","Button"},
                {"UI","UnityEngine.UI","Toggle"},{"UI","UnityEngine.UI","Slider"},{"UI","UnityEngine.UI","InputField"},
                {"UI","UnityEngine.UI","Canvas"},{"UI","UnityEngine","CanvasGroup"},
                {nullptr,nullptr,nullptr}
        };
        std::map<std::string, std::vector<std::pair<std::string,std::string>>> cats;
        for (int i = 0; known[i].cat; i++) {
            Class c(known[i].ns, known[i].name);
            if (c.IsValid()) cats[known[i].cat].push_back({std::string(known[i].ns) + "." + known[i].name, known[i].name});
        }
        for (auto& img : Image::GetImages()) {
            auto* d = img.GetInfo();
            if (!img.IsValid() || !d || !d->name) continue;
            if (strstr(d->name, "Assembly-CSharp") == nullptr && strstr(d->name, "Assembly-CSharp-firstpass") == nullptr) continue;
            for (auto& cls : img.GetClasses(false)) {
                auto* r = cls.GetClass();
                if (!cls.IsValid() || !r || !r->name) continue;
                bool isMono = false;
                for (auto* cur = r; cur; cur = cur->parent)
                    if (cur->name && strcmp(cur->name, "MonoBehaviour") == 0) { isMono = true; break; }
                if (!isMono) continue;
                std::string ns = r->namespaze ? r->namespaze : "";
                std::string fullName = ns.empty() ? r->name : ns + "." + r->name;
                cats["Scripts"].push_back({fullName, r->name});
            }
        }
        for (auto& kv : cats) {
            if (!firstCat) j << ","; firstCat = false;
            j << "\"" << jsEsc(kv.first) << "\":[";
            bool first = true;
            for (auto& p : kv.second) {
                if (!first) j << ","; first = false;
                j << "{\"full\":\"" << jsEsc(p.first) << "\",\"name\":\"" << jsEsc(p.second) << "\"}";
            }
            j << "]";
        }
        j << "}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/catcher/hook",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        std::string asm_ = jsonVal(req.body, "asm");
        std::string ns   = jsonVal(req.body, "ns");
        std::string cls  = jsonVal(req.body, "cls");
        std::string meth = jsonVal(req.body, "method");
        std::string id   = asm_ + "|" + ns + "|" + cls + "|" + meth;
        {
            std::lock_guard<std::mutex> lk(g_catcherMtx);
            if (g_catchers.count(id)) {
                res.set_content(std::string("{\"ok\":true,\"exists\":true,\"id\":\"") + jsEsc(id) + "\"}", "application/json");
                return;
            }
        }
        int slot = -1;
        {
            std::lock_guard<std::mutex> lk(g_catcherSlotMtx);
            for (int i = 0; i < MAX_CATCHERS; i++) {
                if (!g_catcherSlots[i]) { slot = i; break; }
            }
        }
        if (slot < 0) { res.set_content("{\"ok\":false,\"error\":\"max catchers reached\"}", "application/json"); return; }
        auto entry = std::make_shared<CatcherEntry>();
        entry->id = id; entry->asm_ = asm_; entry->ns = ns; entry->cls = cls; entry->method = meth;
        entry->slotIdx = slot;
        Class c(ns.c_str(), cls.c_str(), Image(asm_.c_str()));
        if (!c.IsValid()) { res.set_content("{\"ok\":false,\"error\":\"class not found\"}", "application/json"); return; }
        MethodBase mb = c.GetMethod(meth.c_str());
        if (!mb.IsValid()) { res.set_content("{\"ok\":false,\"error\":\"method not found\"}", "application/json"); return; }
        auto* mi = mb.GetInfo();
        if (!mi || !mi->methodPointer) { res.set_content("{\"ok\":false,\"error\":\"no method pointer\"}", "application/json"); return; }
        entry->retType = typeName(mi->return_type);
        for (int i = 0; i < (int)mi->parameters_count; i++) {
            entry->paramTypes.push_back(mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?");
            entry->paramNames.push_back("arg" + std::to_string(i));
        }
        {
            std::lock_guard<std::mutex> lk(g_catcherSlotMtx);
            g_catcherSlots[slot] = new std::shared_ptr<CatcherEntry>(entry);
        }
        InvokeHook(mb, g_catcherHookFns[slot], g_catcherOrigPtrs[slot]);
        {
            std::lock_guard<std::mutex> lk(g_catcherMtx);
            g_catchers[id] = entry;
        }
        std::ostringstream j;
        j << "{\"ok\":true,\"id\":\"" << jsEsc(id) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/catcher/unhook",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it != g_catchers.end()) {
            it->second->active = false;
            int sl = it->second->slotIdx;
            if (sl >= 0 && sl < MAX_CATCHERS) {
                std::lock_guard<std::mutex> lk2(g_catcherSlotMtx);
                delete g_catcherSlots[sl];
                g_catcherSlots[sl] = nullptr;
                g_catcherOrigPtrs[sl] = nullptr;
            }
            g_catchers.erase(it);
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/catcher/clear",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it != g_catchers.end()) { std::lock_guard<std::mutex> lk2(it->second->callsMtx); it->second->calls.clear(); }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/catcher/list",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& kv : g_catchers) {
            auto& e = kv.second;
            if (!first) j << ","; first = false;
            j << "{\"id\":\"" << jsEsc(e->id) << "\",\"cls\":\"" << jsEsc(e->cls) << "\",\"ns\":\"" << jsEsc(e->ns) << "\",\"method\":\"" << jsEsc(e->method) << "\",\"ret\":\"" << jsEsc(e->retType) << "\",\"active\":" << (e->active?"true":"false") << ",\"count\":";
            { std::lock_guard<std::mutex> lk2(e->callsMtx); j << e->calls.size(); }
            j << ",\"params\":[";
            for (size_t i = 0; i < e->paramTypes.size(); i++) {
                if (i>0) j<<",";
                j << "{\"t\":\"" << jsEsc(e->paramTypes[i]) << "\",\"n\":\"" << jsEsc(e->paramNames[i]) << "\"}";
            }
            j << "]}";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    svr.Get("/api/catcher/calls",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it == g_catchers.end()) { res.set_content("[]", "application/json"); return; }
        auto& e = it->second;
        std::lock_guard<std::mutex> lk2(e->callsMtx);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& c : e->calls) {
            if (!first) j << ","; first = false;
            j << "{\"ts\":" << std::fixed << c.ts << ",\"instance\":\"" << jsEsc(c.instance) << "\",\"args\":[";
            for (size_t i = 0; i < c.args.size(); i++) {
                if (i>0) j<<",";
                j << "{\"t\":\"" << jsEsc(c.args[i].type) << "\",\"v\":" << c.args[i].val << "}";
            }
            j << "],\"ret\":\"" << jsEsc(c.ret) << "\"}";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/lua/exec",[](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string code = jsonVal(req.body, "code");
            if (code.empty()) { res.set_content("{\"output\":\"Error: empty code\"}", "application/json"); return; }
            std::string out = executeLuaBNM(code);
            std::ostringstream j;
            j << "{\"output\":\"" << jsEsc(out) << "\"}";
            res.set_content(j.str(), "application/json");
        } catch (...) {
            res.set_content("{\"output\":\"Error: native crash in executor\"}", "application/json");
        }
    });

    svr.listen("0.0.0.0", g_port);
}

static void OnLoaded() {
    hookDebugLog();
    std::thread(startServer).detach();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, [[maybe_unused]] void* reserved) {
    JNIEnv* env;
    vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    BNM::Loading::AddOnLoadedEvent(OnLoaded);
    BNM::Loading::TryLoadByJNI(env);
    return JNI_VERSION_1_6;
}static std::string jsEsc(const char* s) {
    if (!s) return "";
    std::string o;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
        else                 o += *p;
    }
    return o;
}

static std::string jsEsc(const std::string& s) { return jsEsc(s.c_str()); }

static std::string typeName(const Il2CppType* t) {
    if (!t) return "?";
    Class c(t);
    if (!c.IsValid()) return "?";
    auto* r = c.GetClass();
    if (!r || !r->name) return "?";
    std::string n = r->name;
    if (r->namespaze && strlen(r->namespaze) > 0)
        n = std::string(r->namespaze) + "." + n;
    return n;
}

struct LoopEntry {
    std::string addr;
    std::string asm_;
    std::string ns;
    std::string cls;
    std::string name;
    std::string ftype;
    bool isProp;
    std::string val;
    int intervalMs;
    std::atomic<bool> active{true};
};

static std::mutex g_loopMtx;
static std::map<std::string, std::shared_ptr<LoopEntry>> g_loops;

struct LogEntry {
    std::string msg;
    int level;
    double timestamp;
};
static std::mutex g_logMtx;
static std::vector<LogEntry> g_logBuffer;
static const size_t LOG_MAX = 500;
static std::atomic<bool> g_logHooked{false};

static void addLogEntry(const std::string& msg, int level) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logBuffer.size() >= LOG_MAX) g_logBuffer.erase(g_logBuffer.begin());
    g_logBuffer.push_back({msg, level, nowSeconds()});
}

static void hookDebugLog() {
    if (g_logHooked.exchange(true)) return;
    try {
        Class debugCls("UnityEngine", "Debug");
        if (!debugCls.IsValid()) return;
    } catch(...) {}
}

struct CatcherArg {
    std::string type;
    std::string val;
};

struct CatcherCall {
    double ts;
    std::string instance;
    std::vector<CatcherArg> args;
    std::string ret;
};

struct CatcherEntry {
    std::string id;
    std::string asm_;
    std::string ns;
    std::string cls;
    std::string method;
    std::string retType;
    std::vector<std::string> paramTypes;
    std::vector<std::string> paramNames;
    std::atomic<bool> active{true};
    std::mutex callsMtx;
    std::vector<CatcherCall> calls;
    void* origPtr = nullptr;
    int slotIdx = -1;
    static const size_t MAX_CALLS = 100;
};

static std::mutex g_catcherMtx;
static std::map<std::string, std::shared_ptr<CatcherEntry>> g_catchers;

static std::string logsJson() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& e : g_logBuffer) {
        if (!first) j << ","; first = false;
        j << "{\"msg\":\"" << jsEsc(e.msg) << "\",\"level\":" << e.level << ",\"t\":" << std::fixed << e.timestamp << "}";
    }
    j << "]";
    return j.str();
}

static std::string jsonVal(const std::string& json, const std::string& key) {
    std::string qk = "\"" + key + "\":";
    auto pos = json.find(qk);
    if (pos == std::string::npos) return "";
    pos += qk.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos < json.length() && json[pos] == '"') {
        pos++;
        size_t end = pos;
        bool esc = false;
        while (end < json.length()) {
            if (json[end] == '\\' && !esc) esc = true;
            else if (json[end] == '"' && !esc) break;
            else esc = false;
            end++;
        }
        if (end >= json.length()) return "";
        std::string raw = json.substr(pos, end - pos);
        std::string out;
        for (size_t i = 0; i < raw.length(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.length()) {
                char n = raw[i+1];
                if      (n == '"')  { out += '"';  i++; }
                else if (n == '\\') { out += '\\'; i++; }
                else if (n == 'n')  { out += '\n'; i++; }
                else if (n == 'r')  { out += '\r'; i++; }
                else out += raw[i];
            } else out += raw[i];
        }
        return out;
    }
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) return "";
    std::string v = json.substr(pos, end - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    return v;
}

static MethodBase findObjsMethod() {
    static MethodBase m;
    if (m.IsValid()) return m;
    Class oc("UnityEngine", "Object");
    for (auto& mt : oc.GetMethods(false)) {
        auto* mi = mt.GetInfo();
        if (mi && mi->name && strcmp(mi->name, "FindObjectsOfType") == 0 && mi->parameters_count == 1)
            if (mi->parameters[0] && strstr(typeName(mi->parameters[0]).c_str(), "Type"))
            { m = mt; break; }
    }
    return m;
}

static Il2CppObject* sysTypeOf(const std::string& aqn) {
    Class tc("System", "Type", Image("mscorlib.dll"));
    if (!tc.IsValid()) return nullptr;
    Method<Il2CppObject*> gt(tc.GetMethod("GetType", 1));
    return gt.IsValid() ? gt(CreateMonoString(aqn.c_str())) : nullptr;
}

static bool isObjectAlive(Il2CppObject* obj) {
    if (!obj) return false;
    try {
        Method<bool> m(Class("UnityEngine", "Object").GetMethod("op_Implicit", 1));
        if (m.IsValid()) return m(obj);
        auto* k = obj->klass;
        if (!k || !k->name) return false;
        return true;
    } catch(...) { return false; }
}

static Il2CppObject* resolveComponentByAddr(uintptr_t addr) {
    if (!addr) return nullptr;
    Il2CppObject* obj = (Il2CppObject*)addr;
    if (!isObjectAlive(obj)) return nullptr;
    return obj;
}

static std::string assembliesJson() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& img : Image::GetImages()) {
        auto* d = img.GetInfo();
        if (!img.IsValid() || !d || !d->name) continue;
        if (!first) j << ",";
        first = false;
        j << "\"" << jsEsc(d->name) << "\"";
    }
    j << "]";
    return j.str();
}

static std::string classesJson(const std::string& asm_) {
    Image img(asm_);
    if (!img.IsValid()) return "[]";
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& cls : img.GetClasses(false)) {
        auto* r = cls.GetClass();
        if (!cls.IsValid() || !r || !r->name) continue;
        if (!first) j << ",";
        first = false;
        std::string t = "class";
        if (r->enumtype) t = "enum";
        else if (r->byval_arg.valuetype) t = "struct";
        else if (r->flags & 0x20) t = "interface";
        j << "{\"name\":\"" << jsEsc(r->name) << "\",\"ns\":\"" << jsEsc(r->namespaze ? r->namespaze : "") << "\",\"t\":\"" << t << "\"}";
    }
    j << "]";
    return j.str();
}

static std::string allClassesJson() {
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& img : Image::GetImages()) {
        auto* d = img.GetInfo();
        if (!img.IsValid() || !d || !d->name) continue;
        std::string a = d->name;
        for (auto& cls : img.GetClasses(false)) {
            auto* r = cls.GetClass();
            if (!cls.IsValid() || !r || !r->name) continue;
            if (!first) j << ",";
            first = false;
            std::string t = "class";
            if (r->enumtype) t = "enum";
            else if (r->byval_arg.valuetype) t = "struct";
            else if (r->flags & 0x20) t = "interface";
            j << "{\"name\":\"" << jsEsc(r->name) << "\",\"ns\":\"" << jsEsc(r->namespaze ? r->namespaze : "") << "\",\"t\":\"" << t << "\",\"a\":\"" << jsEsc(a) << "\"}";
        }
    }
    j << "]";
    return j.str();
}

static std::string classDetailJson(const std::string& a, const std::string& ns, const std::string& cn) {
    Class cls(ns.c_str(), cn.c_str(), Image(a.c_str()));
    if (!cls.IsValid()) return "{}";
    auto* r = cls.GetClass();
    if (!r) return "{}";

    std::ostringstream j;
    j << "{\"name\":\"" << jsEsc(cn) << "\",\"ns\":\"" << jsEsc(ns) << "\",\"asm\":\"" << jsEsc(a) << "\",";
    j << (r->parent && r->parent->name ? "\"parent\":\"" + jsEsc(r->parent->name) + "\"," : "\"parent\":null,");

    j << "\"fields\":[";
    bool first = true;
    for (auto& f : cls.GetFields(false)) {
        auto* fi = f.GetInfo();
        if (!f.IsValid() || !fi || !fi->name) continue;
        if (!first) j << ",";
        first = false;
        j << "{\"name\":\"" << jsEsc(fi->name) << "\",\"type\":\"" << jsEsc(typeName(fi->type)) << "\",\"s\":" << (fi->type && (fi->type->attrs & 0x10) ? "true" : "false") << ",\"off\":" << fi->offset << "}";
    }

    j << "],\"methods\":[";
    first = true;
    for (auto& m : cls.GetMethods(false)) {
        auto* mi = m.GetInfo();
        if (!m.IsValid() || !mi || !mi->name) continue;
        if (!first) j << ",";
        first = false;
        char ab[32] = {};
        if (mi->methodPointer) snprintf(ab, sizeof(ab), "%llX", (unsigned long long)(uintptr_t)mi->methodPointer);
        j << "{\"name\":\"" << jsEsc(mi->name) << "\",\"ret\":\"" << jsEsc(typeName(mi->return_type)) << "\",\"s\":" << ((mi->flags & 0x10) ? "true" : "false") << ",\"addr\":\"" << ab << "\",\"params\":[";
        for (int i = 0; i < (int)mi->parameters_count; i++) {
            if (i > 0) j << ",";
            j << "{\"n\":\"arg" << i << "\",\"t\":\"" << jsEsc(mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?") << "\"}";
        }
        j << "]}";
    }

    j << "],\"props\":[";
    first = true;
    for (auto& p : cls.GetProperties(false)) {
        auto* pi = p._data;
        if (!p.IsValid() || !pi || !pi->name) continue;
        if (!first) j << ",";
        first = false;
        j << "{\"name\":\"" << jsEsc(pi->name) << "\",\"type\":\"" << jsEsc(pi->get && pi->get->return_type ? typeName(pi->get->return_type) : "?") << "\",\"g\":" << (pi->get ? "true" : "false") << ",\"s\":" << (pi->set ? "true" : "false") << "}";
    }
    j << "]}";
    return j.str();
}

struct InvokeResult { bool ok; std::string val, err; };

static InvokeResult invokeMethod(const std::string& body) {
    AttachThread();
    InvokeResult res = {false, "", ""};
    std::string a = jsonVal(body, "asm"), ns = jsonVal(body, "ns"), cn = jsonVal(body, "cls"), mn = jsonVal(body, "method");
    bool isStatic = jsonVal(body, "static") == "true" || jsonVal(body, "static") == "1";
    uintptr_t instAddr = (uintptr_t)strtoull(jsonVal(body, "instance").c_str(), nullptr, 16);

    std::vector<std::string> argT, argV;
    auto ap = body.find("\"args\":[");
    if (ap != std::string::npos) {
        ap += 8;
        auto ae = body.find("]", ap);
        if (ae != std::string::npos) {
            std::string ab = body.substr(ap, ae - ap);
            size_t p = 0;
            while (p < ab.size()) {
                auto ob = ab.find("{", p), cb = ab.find("}", ob);
                if (ob == std::string::npos || cb == std::string::npos) break;
                std::string e = "{" + ab.substr(ob + 1, cb - ob - 1) + "}";
                argT.push_back(jsonVal(e, "t"));
                argV.push_back(jsonVal(e, "v"));
                p = cb + 1;
            }
        }
    }

    if (a.empty() || cn.empty() || mn.empty()) { res.err = "Missing asm/cls/method"; return res; }

    Class cls(ns.c_str(), cn.c_str(), Image(a.c_str()));
    if (!cls.IsValid()) { res.err = "Class not found"; return res; }

    MethodBase mb = cls.GetMethod(mn.c_str(), (int)argT.size());
    if (!mb.IsValid()) mb = cls.GetMethod(mn.c_str());
    if (!mb.IsValid()) {
        for (Class cur = cls.GetParent(); cur.IsValid() && cur.GetClass() && !mb.IsValid(); cur = cur.GetParent()) {
            mb = cur.GetMethod(mn.c_str(), (int)argT.size());
            if (!mb.IsValid()) mb = cur.GetMethod(mn.c_str());
        }
    }
    if (!mb.IsValid()) { res.err = "Method not found"; return res; }
    auto* mi = mb.GetInfo();
    if (!mi || !mi->methodPointer) { res.err = "No pointer"; return res; }

    std::vector<void*> runtimeArgs;
    std::vector<int32_t> vi;
    std::vector<int64_t> vi64;
    std::vector<float> vf;
    std::vector<double> vd;
    std::vector<uint8_t> vb;
    std::vector<int16_t> vs16;
    std::vector<Vector3> vv3;
    std::vector<Vector2> vv2;
    std::vector<Color> vc;
    std::vector<Vector4> vv4;

    for (size_t i = 0; i < argT.size(); i++) {
        auto& t = argT[i]; auto& v = argV[i];
        if (t=="System.Int32"||t=="Int32"||t=="int") {
            vi.push_back(std::stoi(v.empty()?"0":v)); runtimeArgs.push_back(&vi.back());
        } else if (t=="System.Int64"||t=="Int64"||t=="long") {
            vi64.push_back(std::stoll(v.empty()?"0":v)); runtimeArgs.push_back(&vi64.back());
        } else if (t=="System.Single"||t=="Single"||t=="float") {
            vf.push_back(std::stof(v.empty()?"0":v)); runtimeArgs.push_back(&vf.back());
        } else if (t=="System.Double"||t=="Double"||t=="double") {
            vd.push_back(std::stod(v.empty()?"0":v)); runtimeArgs.push_back(&vd.back());
        } else if (t=="System.Boolean"||t=="Boolean"||t=="bool") {
            vb.push_back((uint8_t)(v=="true"||v=="1"?1:0)); runtimeArgs.push_back(&vb.back());
        } else if (t=="System.Byte"||t=="Byte"||t=="byte") {
            vb.push_back((uint8_t)std::stoul(v.empty()?"0":v)); runtimeArgs.push_back(&vb.back());
        } else if (t=="System.Int16"||t=="Int16"||t=="short") {
            vs16.push_back((int16_t)std::stoi(v.empty()?"0":v)); runtimeArgs.push_back(&vs16.back());
        } else if (t=="UnityEngine.Vector3"||t=="Vector3") {
            float x=0,y=0,z=0; sscanf(v.c_str(),"[%f,%f,%f]",&x,&y,&z); vv3.push_back({x,y,z}); runtimeArgs.push_back(&vv3.back());
        } else if (t=="UnityEngine.Vector2"||t=="Vector2") {
            float x=0,y=0; sscanf(v.c_str(),"[%f,%f]",&x,&y); vv2.push_back({x,y}); runtimeArgs.push_back(&vv2.back());
        } else if (t=="UnityEngine.Color"||t=="Color") {
            float r=1,g=1,b=1,aa=1; sscanf(v.c_str(),"[%f,%f,%f,%f]",&r,&g,&b,&aa); vc.push_back({r,g,b,aa}); runtimeArgs.push_back(&vc.back());
        } else if (t=="UnityEngine.Vector4"||t=="Vector4"||t=="UnityEngine.Quaternion"||t=="Quaternion") {
            float x=0,y=0,z=0,w=0; sscanf(v.c_str(),"[%f,%f,%f,%f]",&x,&y,&z,&w); vv4.push_back({x,y,z,w}); runtimeArgs.push_back(&vv4.back());
        } else if (t=="System.String"||t=="String"||t=="string") {
            runtimeArgs.push_back(CreateMonoString(v));
        } else {
            runtimeArgs.push_back((void*)strtoull(v.empty()?"0":v.c_str(), nullptr, 16));
        }
    }

    void* inst = isStatic ? nullptr : (void*)instAddr;
    std::string rtn = typeName(mi->return_type);
    std::ostringstream out;

    Il2CppException* exc = nullptr;
    void* ret = BNM::Internal::il2cppMethods.il2cpp_runtime_invoke(mi, inst, runtimeArgs.empty() ? nullptr : runtimeArgs.data(), &exc);

    if (exc) {
        BNM::Structures::Mono::String* excMsg = nullptr;
        try {
            auto excCls = Class(((Il2CppObject*)exc)->klass);
            if (excCls.IsValid()) {
                auto msgMethod = excCls.GetMethod("get_Message", 0);
                if (msgMethod.IsValid()) {
                    auto* msgMi = msgMethod.GetInfo();
                    if (msgMi && msgMi->methodPointer) {
                        Il2CppException* inner = nullptr;
                        auto* msgRet = (Il2CppObject*)BNM::Internal::il2cppMethods.il2cpp_runtime_invoke(msgMi, exc, nullptr, &inner);
                        if (msgRet && !inner) excMsg = (BNM::Structures::Mono::String*)msgRet;
                    }
                }
            }
        } catch(...) {}
        res.err = excMsg ? excMsg->str() : "IL2CPP exception";
        return res;
    }

    if (rtn=="System.Void"||rtn=="Void"||rtn=="void"||rtn=="?") {
        out << "void";
    } else if (rtn=="System.Single"||rtn=="Single"||rtn=="float") {
        float v = ret ? *(float*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Double"||rtn=="Double"||rtn=="double") {
        double v = ret ? *(double*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Int32"||rtn=="Int32"||rtn=="int") {
        int v = ret ? *(int*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Int64"||rtn=="Int64"||rtn=="long") {
        int64_t v = ret ? *(int64_t*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << v;
    } else if (rtn=="System.Boolean"||rtn=="Boolean"||rtn=="bool") {
        bool v = ret ? *(bool*)((uint8_t*)ret + sizeof(Il2CppObject)) : false;
        out << (v?"true":"false");
    } else if (rtn=="System.Byte"||rtn=="Byte"||rtn=="byte") {
        uint8_t v = ret ? *(uint8_t*)((uint8_t*)ret + sizeof(Il2CppObject)) : 0;
        out << (int)v;
    } else if (rtn=="UnityEngine.Vector3"||rtn=="Vector3") {
        Vector3 v = ret ? *(Vector3*)((uint8_t*)ret + sizeof(Il2CppObject)) : Vector3{0,0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f, %.4f]",v.x,v.y,v.z); out<<buf;
    } else if (rtn=="UnityEngine.Vector2"||rtn=="Vector2") {
        Vector2 v = ret ? *(Vector2*)((uint8_t*)ret + sizeof(Il2CppObject)) : Vector2{0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f]",v.x,v.y); out<<buf;
    } else if (rtn=="UnityEngine.Color"||rtn=="Color") {
        Color v = ret ? *(Color*)((uint8_t*)ret + sizeof(Il2CppObject)) : Color{0,0,0,0};
        char buf[128]; snprintf(buf,sizeof(buf),"[%.4f, %.4f, %.4f, %.4f]",v.r,v.g,v.b,v.a); out<<buf;
    } else if (rtn=="System.String"||rtn=="String"||rtn=="string") {
        auto* s = (BNM::Structures::Mono::String*)ret;
        out << (s ? s->str() : "null");
    } else {
        char buf[64]; snprintf(buf,sizeof(buf),"0x%llX",(unsigned long long)(uintptr_t)ret);
        out << rtn << " @ " << buf;
    }
    res.ok = true; res.val = out.str();
    return res;
}

static std::string instancesJson(const std::string& a, const std::string& ns, const std::string& cn) {
    AttachThread();
    std::string full = ns.empty() ? cn : ns + "." + cn;
    std::string aNoExt = a;
    auto dp = aNoExt.find(".dll");
    if (dp != std::string::npos) aNoExt = aNoExt.substr(0, dp);

    Il2CppObject* st = sysTypeOf(full + ", " + aNoExt);
    if (!st) st = sysTypeOf(full);
    if (!st) return "{\"error\":\"Class not found / System.Type missing\"}";

    auto fm = findObjsMethod();
    if (!fm.IsValid()) return "{\"error\":\"FindObjectsOfType missing\"}";

    std::ostringstream j;
    j << "{\"instances\":[";
    try {
        auto* arr = Method<Array<Il2CppObject*>*>(fm)(st);
        if (arr && arr->capacity > 0) {
            bool first = true;
            Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
            for (int i = 0; i < arr->capacity; i++) {
                Il2CppObject* obj = arr->m_Items[i];
                if (!obj) continue;
                if (!first) j << ",";
                first = false;
                std::string name = "obj";
                if (gn.IsValid()) { auto* s = gn[obj](); if (s) name = s->str(); }
                char addr[32]; snprintf(addr, sizeof(addr), "%llX", (unsigned long long)(uintptr_t)obj);
                j << "{\"addr\":\"" << addr << "\",\"name\":\"" << jsEsc(name) << "\"}";
            }
        }
    } catch(...) {}
    j << "]}";
    return j.str();
}

static std::string sceneJson() {
    AttachThread();
    auto fm = findObjsMethod();
    if (!fm.IsValid()) return "[]";

    Il2CppObject* st = sysTypeOf("UnityEngine.GameObject, UnityEngine.CoreModule");
    if (!st) st = sysTypeOf("UnityEngine.GameObject, UnityEngine");
    if (!st) st = sysTypeOf("UnityEngine.GameObject");
    if (!st) return "[]";

    Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
    Method<bool> ga(Class("UnityEngine","GameObject").GetMethod("get_activeSelf"));
    Method<Il2CppObject*> gt(Class("UnityEngine","GameObject").GetMethod("get_transform"));
    Method<Il2CppObject*> tp(Class("UnityEngine","Transform").GetMethod("get_parent"));
    Method<Il2CppObject*> cg(Class("UnityEngine","Component").GetMethod("get_gameObject"));

    std::ostringstream j;
    j << "[";
    try {
        auto* arr = Method<Array<Il2CppObject*>*>(fm)(st);
        if (arr && arr->capacity > 0) {
            bool first = true;
            for (int i = 0; i < arr->capacity; i++) {
                Il2CppObject* go = arr->m_Items[i];
                if (!go) continue;
                std::string name = "Unknown";
                bool active = false;
                if (gn.IsValid()) { auto* s = gn[go](); if (s) name = s->str(); }
                if (ga.IsValid()) active = ga[go]();
                uintptr_t par = 0;
                if (gt.IsValid() && tp.IsValid() && cg.IsValid()) {
                    Il2CppObject* tr = gt[go]();
                    if (tr) { Il2CppObject* pt = tp[tr](); if (pt) { Il2CppObject* pg = cg[pt](); if (pg) par = (uintptr_t)pg; } }
                }
                if (!first) j << ",";
                first = false;
                j << "{\"addr\":\"" << std::hex << (uintptr_t)go << "\",\"name\":\"" << jsEsc(name) << "\",\"active\":" << (active?"true":"false") << ",\"parent\":\"" << std::hex << par << "\"}";
            }
        }
    } catch(...) {}
    j << "]";
    return j.str();
}

static std::string readField(const std::string& ft, Class& cls, Il2CppObject* inst, const std::string& name, MethodBase* getter, bool isStatic, bool& ok) {
    ok = false;
    if (!inst && !isStatic) return "";
    try {
        if (ft=="System.Single"||ft=="Single"||ft=="float") {
            float v = 0;
            if (getter) { Method<float> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<float> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Double"||ft=="Double"||ft=="double") {
            double v = 0;
            if (getter) { Method<double> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<double> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int32"||ft=="Int32"||ft=="int") {
            int v = 0;
            if (getter) { Method<int> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt32"||ft=="UInt32"||ft=="uint") {
            uint32_t v = 0;
            if (getter) { Method<uint32_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint32_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int64"||ft=="Int64"||ft=="long") {
            int64_t v = 0;
            if (getter) { Method<int64_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt64"||ft=="UInt64"||ft=="ulong") {
            uint64_t v = 0;
            if (getter) { Method<uint64_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Int16"||ft=="Int16"||ft=="short") {
            int16_t v = 0;
            if (getter) { Method<int16_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.UInt16"||ft=="UInt16"||ft=="ushort") {
            uint16_t v = 0;
            if (getter) { Method<uint16_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Byte"||ft=="Byte"||ft=="byte") {
            uint8_t v = 0;
            if (getter) { Method<uint8_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<uint8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.SByte"||ft=="SByte"||ft=="sbyte") {
            int8_t v = 0;
            if (getter) { Method<int8_t> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="System.Boolean"||ft=="Boolean"||ft=="bool") {
            bool v = false;
            if (getter) { Method<bool> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<bool> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return v ? "true" : "false";
        } else if (ft=="System.String"||ft=="String"||ft=="string") {
            BNM::Structures::Mono::String* s = nullptr;
            if (getter) { Method<BNM::Structures::Mono::String*> m(*getter); if(!isStatic) m.SetInstance(inst); s = m(); }
            else { Field<BNM::Structures::Mono::String*> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);s=f();} }
            ok = true; return s ? "\"" + jsEsc(s->str()) + "\"" : "\"\"";
        } else if (ft=="UnityEngine.Vector3"||ft=="Vector3") {
            Vector3 v = {0,0,0};
            if (getter) { Method<Vector3> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector3> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return "[" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "]";
        } else if (ft=="UnityEngine.Color"||ft=="Color") {
            Color c = {0,0,0,0};
            if (getter) { Method<Color> m(*getter); if(!isStatic) m.SetInstance(inst); c = m(); }
            else { Field<Color> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);c=f();} }
            ok = true; return "[" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a) + "]";
        } else if (ft=="UnityEngine.Vector2"||ft=="Vector2") {
            Vector2 v = {0,0};
            if (getter) { Method<Vector2> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector2> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return "[" + std::to_string(v.x) + "," + std::to_string(v.y) + "]";
        } else if (ft=="UnityEngine.Vector4"||ft=="Vector4"||ft=="UnityEngine.Quaternion"||ft=="Quaternion") {
            Vector4 v = {0,0,0,0};
            if (getter) { Method<Vector4> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Vector4> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::string("{\"x\":") + std::to_string(v.x) + ",\"y\":" + std::to_string(v.y) + ",\"z\":" + std::to_string(v.z) + ",\"w\":" + std::to_string(v.w) + "}";
        } else if (ft=="UnityEngine.LayerMask"||ft=="LayerMask") {
            int v = 0;
            if (getter) { Method<int> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true; return std::to_string(v);
        } else if (ft=="UnityEngine.Rect"||ft=="Rect") {
            Rect v = {0,0,0,0};
            if (getter) { Method<Rect> m(*getter); if(!isStatic) m.SetInstance(inst); v = m(); }
            else { Field<Rect> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);v=f();} }
            ok = true;
            return std::string("{\"x\":") + std::to_string(v.x) + ",\"y\":" + std::to_string(v.y) + ",\"w\":" + std::to_string(v.w) + ",\"h\":" + std::to_string(v.h) + "}";
        }
    } catch(...) {}
    return "";
}

static void writeField(const std::string& ft, Class& cls, Il2CppObject* inst, const std::string& name, MethodBase* setter, const std::string& v, bool isStatic = false) {
    if (!inst && !isStatic) return;
    try {
        if (ft=="System.Single"||ft=="Single"||ft=="float") {
            float val = std::stof(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<float> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Double"||ft=="Double"||ft=="double") {
            double val = std::stod(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<double> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int32"||ft=="Int32"||ft=="int") {
            int val = std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt32"||ft=="UInt32"||ft=="uint") {
            uint32_t val = (uint32_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint32_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int64"||ft=="Int64"||ft=="long") {
            int64_t val = std::stoll(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt64"||ft=="UInt64"||ft=="ulong") {
            uint64_t val = std::stoull(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint64_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Int16"||ft=="Int16"||ft=="short") {
            int16_t val = (int16_t)std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.UInt16"||ft=="UInt16"||ft=="ushort") {
            uint16_t val = (uint16_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint16_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Byte"||ft=="Byte"||ft=="byte") {
            uint8_t val = (uint8_t)std::stoul(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<uint8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.SByte"||ft=="SByte"||ft=="sbyte") {
            int8_t val = (int8_t)std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int8_t> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.Boolean"||ft=="Boolean"||ft=="bool") {
            bool val = v=="true"||v=="1";
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<bool> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="System.String"||ft=="String"||ft=="string") {
            auto* s = CreateMonoString(v.c_str());
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(s); }
            else { Field<BNM::Structures::Mono::String*> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=s;} }
        } else if (ft=="UnityEngine.Vector3"||ft=="Vector3") {
            float x=0,y=0,z=0; sscanf(v.c_str(),"[%f,%f,%f]",&x,&y,&z); Vector3 val={x,y,z};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector3> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Color"||ft=="Color") {
            float r=1,g=1,b=1,a=1; sscanf(v.c_str(),"[%f,%f,%f,%f]",&r,&g,&b,&a); Color val={r,g,b,a};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Color> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Vector2"||ft=="Vector2") {
            float x=0,y=0; sscanf(v.c_str(),"[%f,%f]",&x,&y); Vector2 val={x,y};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector2> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Vector4"||ft=="Vector4"||ft=="UnityEngine.Quaternion"||ft=="Quaternion") {
            float x=0,y=0,z=0,w=0;
            sscanf(v.c_str(),"{\"x\":%f,\"y\":%f,\"z\":%f,\"w\":%f}",&x,&y,&z,&w);
            Vector4 val={x,y,z,w};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Vector4> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.LayerMask"||ft=="LayerMask") {
            int val = std::stoi(v);
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<int> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        } else if (ft=="UnityEngine.Rect"||ft=="Rect") {
            float x=0,y=0,w=0,h=0;
            sscanf(v.c_str(),"{\"x\":%f,\"y\":%f,\"w\":%f,\"h\":%f}",&x,&y,&w,&h);
            Rect val={x,y,w,h};
            if (setter) { Method<void> m(*setter); if(!isStatic) m.SetInstance(inst); m(val); }
            else { Field<Rect> f=cls.GetField(name.c_str()); if(f.IsValid()){if(!isStatic)f.SetInstance(inst);f=val;} }
        }
    } catch(...) {}
}

static std::string goInfoJson(uintptr_t addr) {
    AttachThread();
    if (!addr) return "{}";
    Il2CppObject* go = (Il2CppObject*)addr;
    if (!isObjectAlive(go)) return "{\"stale\":true}";
    std::string name = "Unknown";
    bool active = false;

    Class goCls("UnityEngine","GameObject");
    Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
    Method<bool> ga(goCls.GetMethod("get_activeSelf"));
    Method<Il2CppObject*> gt(goCls.GetMethod("get_transform"));

    try {
        if (gn.IsValid()) { auto* s=gn[go](); if(s) name=s->str(); }
        if (ga.IsValid()) active = ga[go]();
    } catch(...) {}

    std::ostringstream j;
    j << "{\"addr\":\"" << std::hex << addr << "\",\"name\":\"" << jsEsc(name) << "\",\"active\":" << (active?"true":"false") << ",\"transform\":{";
    try {
        if (gt.IsValid()) {
            Il2CppObject* tr = gt[go]();
            if (tr) {
                Method<Vector3> gp(Class("UnityEngine","Transform").GetMethod("get_localPosition"));
                Method<Vector3> gr(Class("UnityEngine","Transform").GetMethod("get_localEulerAngles"));
                Method<Vector3> gs(Class("UnityEngine","Transform").GetMethod("get_localScale"));
                Vector3 p=gp.IsValid()?gp[tr]():Vector3{0,0,0};
                Vector3 r=gr.IsValid()?gr[tr]():Vector3{0,0,0};
                Vector3 s=gs.IsValid()?gs[tr]():Vector3{0,0,0};
                j << "\"addr\":\"" << std::hex << (uintptr_t)tr << "\",\"p\":[" << p.x << "," << p.y << "," << p.z << "],\"r\":[" << r.x << "," << r.y << "," << r.z << "],\"s\":[" << s.x << "," << s.y << "," << s.z << "]";
            }
        }
    } catch(...) {}
    j << "},\"scripts\":[";

    try {
        Class componentCls("UnityEngine", "Component");
        Il2CppObject* compReflType = nullptr;
        if (componentCls.IsValid()) compReflType = (Il2CppObject*)componentCls.GetMonoType();
        if (!compReflType) compReflType = sysTypeOf("UnityEngine.Component, UnityEngine.CoreModule");
        if (!compReflType) compReflType = sysTypeOf("UnityEngine.Component");
        Array<Il2CppObject*>* comps = nullptr;
        if (!comps && compReflType) {
            for (auto& mt : goCls.GetMethods(true)) {
                auto* mi = mt.GetInfo();
                if (!mi || !mi->name || mi->is_generic) continue;
                if (strcmp(mi->name, "GetComponents") != 0) continue;
                if (mi->parameters_count == 1) {
                    try {
                        Method<Array<Il2CppObject*>*> m(mt);
                        comps = m[go](compReflType);
                    } catch(...) { LOGE("goInfoJson: GetComponents(Type) threw"); }
                    if (comps && comps->capacity > 0) break;
                    comps = nullptr;
                }
            }
        }
        if (!comps && compReflType) {
            for (auto& mt : goCls.GetMethods(true)) {
                auto* mi = mt.GetInfo();
                if (!mi || !mi->name) continue;
                if (strcmp(mi->name, "GetComponentsInternal") != 0) continue;
                LOGD("goInfoJson: found GetComponentsInternal with %d params", mi->parameters_count);
                if (mi->parameters_count >= 2) {
                    try {
                        if (mi->parameters_count == 6) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, true, false, true, false, nullptr);
                            LOGD("goInfoJson: GetComponentsInternal(6) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        } else if (mi->parameters_count == 5) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, true, false, true, false);
                            LOGD("goInfoJson: GetComponentsInternal(5) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        } else if (mi->parameters_count == 4) {
                            Method<Array<Il2CppObject*>*> m(mt);
                            comps = m[go](compReflType, false, true, false);
                            LOGD("goInfoJson: GetComponentsInternal(4) returned %p, count=%d", comps, comps ? (int)comps->capacity : -1);
                        }
                    } catch(...) { LOGE("goInfoJson: GetComponentsInternal threw"); }
                    if (comps && comps->capacity > 0) break;
                    comps = nullptr;
                }
            }
        }
        if (!comps) {
            LOGD("goInfoJson: falling back to FindObjectsOfType");
            Class monoCls("UnityEngine", "MonoBehaviour");
            Il2CppObject* monoType = nullptr;
            if (monoCls.IsValid()) monoType = (Il2CppObject*)monoCls.GetMonoType();
            if (!monoType) monoType = sysTypeOf("UnityEngine.MonoBehaviour, UnityEngine.CoreModule");
            if (!monoType) monoType = sysTypeOf("UnityEngine.MonoBehaviour");

            auto fom = findObjsMethod();
            Method<Il2CppObject*> compGetGO(Class("UnityEngine","Component").GetMethod("get_gameObject"));

            if (fom.IsValid() && monoType && compGetGO.IsValid()) {
                auto* allMonos = Method<Array<Il2CppObject*>*>(fom)(monoType);
                LOGD("goInfoJson: FindObjectsOfType(MonoBehaviour) count=%d", allMonos ? (int)allMonos->capacity : -1);
                if (allMonos && allMonos->capacity > 0) {
                    for (int i = 0; i < allMonos->capacity; i++) {
                        Il2CppObject* c = allMonos->m_Items[i];
                        if (!c) continue;
                        try {
                            Il2CppObject* cgo = compGetGO[c]();
                            if ((uintptr_t)cgo != addr) allMonos->m_Items[i] = nullptr;
                        } catch(...) { allMonos->m_Items[i] = nullptr; }
                    }
                    comps = allMonos;
                }
            }
        }

        LOGD("goInfoJson: final comps=%p, count=%d", comps, comps ? (int)comps->capacity : -1);

        static const char* kStopAtComponent[] = {
                "Component", "Object", nullptr
        };
        static const char* kSkipProps[] = {
                "isActiveAndEnabled", "transform", "gameObject", "tag", "name",
                "hideFlags", "rigidbody", "rigidbody2D", "camera", "light",
                "animation", "constantForce", "renderer", "audio", "networkView",
                "collider", "collider2D", "hingeJoint", "particleSystem", nullptr
        };

        auto isStopClass = [](const char* n) -> bool {
            for (auto** s = kStopAtComponent; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };
        auto isSkipProp = [](const char* n) -> bool {
            for (auto** s = kSkipProps; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };

        auto isKnownBuiltinBase = [](const char* n) -> bool {
            static const char* builtins[] = {
                    "MonoBehaviour", "Behaviour",
                    "Collider", "Collider2D",
                    "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "TerrainCollider", "WheelCollider",
                    "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "EdgeCollider2D", "CapsuleCollider2D", "CompositeCollider2D",
                    "Rigidbody", "Rigidbody2D",
                    "Renderer", "MeshRenderer", "SkinnedMeshRenderer", "SpriteRenderer", "LineRenderer", "TrailRenderer", "ParticleSystemRenderer",
                    "MeshFilter",
                    "Animator", "Animation",
                    "AudioSource", "AudioListener",
                    "Camera", "Light", "LensFlare", "Projector",
                    "Canvas", "CanvasGroup", "CanvasRenderer",
                    "RectTransform",
                    "Text", "Image", "RawImage", "Button", "Toggle", "Slider", "Scrollbar", "Dropdown", "InputField", "ScrollRect", "Mask", "RectMask2D",
                    "TMP_Text", "TextMeshPro", "TextMeshProUGUI",
                    "NavMeshAgent", "NavMeshObstacle",
                    "ParticleSystem",
                    "Joint", "HingeJoint", "FixedJoint", "SpringJoint", "CharacterJoint", "ConfigurableJoint",
                    "Joint2D", "HingeJoint2D", "FixedJoint2D", "SpringJoint2D", "WheelJoint2D", "SliderJoint2D",
                    "CharacterController",
                    "LODGroup",
                    "OcclusionArea", "OcclusionPortal",
                    "NetworkView",
                    "ConstantForce",
                    nullptr
            };
            for (auto** s = builtins; *s; s++)
                if (strcmp(n, *s) == 0) return true;
            return false;
        };

        if (comps && comps->capacity > 0) {
            bool firstScript = true;

            for (int i = 0; i < comps->capacity; i++) {
                Il2CppObject* comp = comps->m_Items[i];
                if (!comp || !comp->klass) continue;

                auto* klass = comp->klass;

                bool isSupported = false;
                std::string compCategory = "script";
                for (auto* cur = klass; cur; cur = cur->parent) {
                    if (!cur->name) continue;
                    if (strcmp(cur->name, "MonoBehaviour") == 0) { isSupported = true; compCategory = "script"; break; }
                    if (isKnownBuiltinBase(cur->name) && strcmp(cur->name, "MonoBehaviour") != 0 && strcmp(cur->name, "Behaviour") != 0) {
                        isSupported = true; compCategory = "builtin"; break;
                    }
                    if (strcmp(cur->name, "Component") == 0 || strcmp(cur->name, "Object") == 0) break;
                }
                if (!isSupported) continue;

                std::string compName = klass->name ? klass->name : "Unknown";
                std::string compNs = klass->namespaze ? klass->namespaze : "";
                char compAddr[32]; snprintf(compAddr, sizeof(compAddr), "%llX", (unsigned long long)(uintptr_t)comp);
                std::string compAsm = (klass->image && klass->image->name) ? klass->image->name : "";

                bool hasEnabledProp = false;
                bool enabled = false;
                for (auto* cur = klass; cur; cur = cur->parent) {
                    if (!cur->name) continue;
                    if (strcmp(cur->name, "Behaviour") == 0 || strcmp(cur->name, "Renderer") == 0 ||
                        strcmp(cur->name, "Collider") == 0 || strcmp(cur->name, "Collider2D") == 0) {
                        hasEnabledProp = true; break;
                    }
                    if (strcmp(cur->name, "Component") == 0) break;
                }
                if (hasEnabledProp) {
                    Method<bool> getEnabled(Class("UnityEngine","Behaviour").GetMethod("get_enabled"));
                    if (!getEnabled.IsValid()) {
                        Class compClsCheck(klass);
                        auto ep = compClsCheck.GetProperty("enabled");
                        if (ep.IsValid() && ep._data && ep._data->get) {
                            MethodBase egetter(ep._data->get);
                            Method<bool> m(egetter); m.SetInstance(comp);
                            try { enabled = m(); } catch(...) {}
                        }
                    } else {
                        try { enabled = getEnabled[comp](); } catch(...) {}
                    }
                }

                if (!firstScript) j << ",";
                firstScript = false;
                j << "{\"addr\":\"" << compAddr << "\",\"name\":\"" << jsEsc(compName) << "\",\"ns\":\"" << jsEsc(compNs) << "\",\"asm\":\"" << jsEsc(compAsm) << "\",\"category\":\"" << compCategory << "\",\"enabled\":" << (enabled?"true":"false") << ",\"fields\":[";

                LOGD("goInfoJson: found component '%s' category='%s' at %s", compName.c_str(), compCategory.c_str(), compAddr);

                try {
                    Class compCls(klass);
                    bool firstF = true;
                    std::vector<std::string> seen;

                    for (Class cur = compCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                        auto* cn_ = cur.GetClass()->name;
                        if (!cn_) break;
                        if (isStopClass(cn_)) break;

                        for (auto& f : cur.GetFields(false)) {
                            try {
                                auto* fi = f.GetInfo();
                                if (!fi || !fi->type || (fi->type->attrs & 0x10)) continue;
                                std::string fn = fi->name;
                                bool dup = false; for (auto& s : seen) if (s==fn) dup=true;
                                if (dup) continue; seen.push_back(fn);
                                std::string ft = typeName(fi->type);
                                bool ok = false;
                                std::string vs = readField(ft, cur, comp, fn, nullptr, false, ok);
                                if (ok) {
                                    if (!firstF) j << ","; firstF = false;
                                    j << "{\"name\":\"" << jsEsc(fn) << "\",\"type\":\"" << jsEsc(ft) << "\",\"val\":" << vs << ",\"isProp\":false,\"canWrite\":true}";
                                }
                            } catch(...) {}
                        }

                        for (auto& p : cur.GetProperties(false)) {
                            try {
                                auto* pi = p._data;
                                if (!pi || !pi->get) continue;
                                std::string pn = pi->name;
                                if (isSkipProp(pn.c_str())) continue;
                                bool dup = false; for (auto& s : seen) if (s==pn) dup=true;
                                if (dup) continue; seen.push_back(pn);
                                std::string pt = typeName(pi->get->return_type);
                                bool ok = false;
                                MethodBase getter(pi->get);
                                std::string vs = readField(pt, cur, comp, pn, &getter, false, ok);
                                if (ok) {
                                    if (!firstF) j << ","; firstF = false;
                                    j << "{\"name\":\"" << jsEsc(pn) << "\",\"type\":\"" << jsEsc(pt) << "\",\"val\":" << vs << ",\"isProp\":true,\"canWrite\":" << (pi->set?"true":"false") << "}";
                                }
                            } catch(...) {}
                        }
                    }
                } catch(...) {}
                j << "]}";
            }
        }
    } catch(...) {
        LOGE("goInfoJson: exception in script enumeration");
    }
    j << "]}";
    return j.str();
}

static void applyLoopEntry(const std::shared_ptr<LoopEntry>& e) {
    try {
        uintptr_t addr = (uintptr_t)strtoull(e->addr.c_str(), nullptr, 16);
        if (!addr) return;
        Il2CppObject* obj = resolveComponentByAddr(addr);
        if (!obj) { e->active = false; return; }
        for (Class cur(e->ns.c_str(), e->cls.c_str(), Image(e->asm_.c_str())); cur.IsValid(); cur = cur.GetParent()) {
            if (e->isProp) {
                auto p = cur.GetProperty(e->name.c_str());
                if (p.IsValid() && p._data && p._data->set) {
                    MethodBase s(p._data->set);
                    writeField(e->ftype, cur, obj, e->name, &s, e->val);
                    return;
                }
            } else {
                auto f = cur.GetField(e->name.c_str());
                if (f.IsValid()) { writeField(e->ftype, cur, obj, e->name, nullptr, e->val); return; }
            }
        }
    } catch(...) {}
}

static std::string loopListJson() {
    std::lock_guard<std::mutex> lk(g_loopMtx);
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (auto& kv : g_loops) {
        if (!first) j << ","; first = false;
        auto& e = kv.second;
        j << "{\"id\":\"" << jsEsc(kv.first) << "\",\"cls\":\"" << jsEsc(e->cls) << "\",\"name\":\"" << jsEsc(e->name) << "\",\"val\":\"" << jsEsc(e->val) << "\",\"interval\":" << e->intervalMs << ",\"active\":" << (e->active?"true":"false") << "}";
    }
    j << "]";
    return j.str();
}

#define MAX_CATCHERS 64
static std::shared_ptr<CatcherEntry>* g_catcherSlots[MAX_CATCHERS] = {};
static int g_catcherSlotCount = 0;
static std::mutex g_catcherSlotMtx;
static void (*g_catcherOrigPtrs[MAX_CATCHERS])(Il2CppObject*) = {};

template<int N>
static void catcherHook(Il2CppObject* self) {
    std::shared_ptr<CatcherEntry>* slot = g_catcherSlots[N];
    if (slot && *slot && (*slot)->active) {
        CatcherCall call;
        call.ts = nowSeconds();
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)(uintptr_t)self);
        call.instance = buf;
        if (self && self->klass) {
            try {
                Method<BNM::Structures::Mono::String*> gn(Class("UnityEngine","Object").GetMethod("get_name"));
                if (gn.IsValid()) { auto* s = gn[self](); if (s) call.instance += " (" + s->str() + ")"; }
            } catch(...) {}
        }
        std::lock_guard<std::mutex> lk((*slot)->callsMtx);
        if ((*slot)->calls.size() >= CatcherEntry::MAX_CALLS) (*slot)->calls.erase((*slot)->calls.begin());
        (*slot)->calls.push_back(std::move(call));
    }
    if (g_catcherOrigPtrs[N]) g_catcherOrigPtrs[N](self);
}

typedef void(*CatcherHookFn)(Il2CppObject*);
static CatcherHookFn g_catcherHookFns[MAX_CATCHERS] = {
        catcherHook<0>,catcherHook<1>,catcherHook<2>,catcherHook<3>,catcherHook<4>,
        catcherHook<5>,catcherHook<6>,catcherHook<7>,catcherHook<8>,catcherHook<9>,
        catcherHook<10>,catcherHook<11>,catcherHook<12>,catcherHook<13>,catcherHook<14>,
        catcherHook<15>,catcherHook<16>,catcherHook<17>,catcherHook<18>,catcherHook<19>,
        catcherHook<20>,catcherHook<21>,catcherHook<22>,catcherHook<23>,catcherHook<24>,
        catcherHook<25>,catcherHook<26>,catcherHook<27>,catcherHook<28>,catcherHook<29>,
        catcherHook<30>,catcherHook<31>,catcherHook<32>,catcherHook<33>,catcherHook<34>,
        catcherHook<35>,catcherHook<36>,catcherHook<37>,catcherHook<38>,catcherHook<39>,
        catcherHook<40>,catcherHook<41>,catcherHook<42>,catcherHook<43>,catcherHook<44>,
        catcherHook<45>,catcherHook<46>,catcherHook<47>,catcherHook<48>,catcherHook<49>,
        catcherHook<50>,catcherHook<51>,catcherHook<52>,catcherHook<53>,catcherHook<54>,
        catcherHook<55>,catcherHook<56>,catcherHook<57>,catcherHook<58>,catcherHook<59>,
        catcherHook<60>,catcherHook<61>,catcherHook<62>,catcherHook<63>
};

static void startServer() {
    httplib::Server svr;

    svr.Get("/",[](const httplib::Request&, httplib::Response& res) {
        int n = 0;
        for (auto& img : Image::GetImages()) if (img.IsValid()) n++;
        res.set_content(GetExplorerHTML(n), "text/html");
    });

    svr.Get("/api/assemblies",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(assembliesJson(), "application/json");
    });

    svr.Get("/api/classes",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(classesJson(req.get_param_value("a")), "application/json");
    });

    svr.Get("/api/allclasses",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(allClassesJson(), "application/json");
    });

    svr.Get("/api/class",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(classDetailJson(req.get_param_value("a"), req.get_param_value("ns"), req.get_param_value("n")), "application/json");
    });

    svr.Get("/api/instances",[](const httplib::Request& req, httplib::Response& res) {
        res.set_content(instancesJson(req.get_param_value("a"), req.get_param_value("ns"), req.get_param_value("n")), "application/json");
    });

    svr.Get("/api/scene",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(sceneJson(), "application/json");
    });

    svr.Get("/api/scene/inspect",[](const httplib::Request& req, httplib::Response& res) {
        auto s = req.get_param_value("addr");
        res.set_content(s.empty() ? "{}" : goInfoJson((uintptr_t)strtoull(s.c_str(), nullptr, 16)), "application/json");
    });

    svr.Get("/api/controller/inspect",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(req.get_param_value("addr").c_str(), nullptr, 16);
        std::string a = req.get_param_value("asm"), ns = req.get_param_value("ns"), cn = req.get_param_value("cls");

        Class startCls(ns.c_str(), cn.c_str(), Image(a.c_str()));
        if (!startCls.IsValid()) { res.set_content("{}", "application/json"); return; }

        std::ostringstream j;
        j << "{\"addr\":\"" << std::hex << addr << "\",\"name\":\"" << jsEsc(cn) << "\",\"fields\":[";

        bool firstF = true;
        std::vector<std::string> seen;

        for (Class cur = startCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
            auto* cn_ = cur.GetClass()->name;
            if (!cn_) break;
            if (strcmp(cn_, "Object") == 0 && cur.GetClass()->namespaze && strcmp(cur.GetClass()->namespaze, "System") == 0) break;

            for (auto& f : cur.GetFields(false)) {
                try {
                    auto* fi = f.GetInfo();
                    if (!fi || !fi->type) continue;
                    bool fStatic = (fi->type->attrs & 0x10) != 0;
                    if (!fStatic && !addr) continue;
                    std::string fn = fi->name;
                    bool dup = false; for (auto& s : seen) if (s==fn) dup=true;
                    if (dup) continue; seen.push_back(fn);
                    std::string ft = typeName(fi->type);
                    bool ok = false;
                    std::string vs = readField(ft, cur, fStatic ? nullptr : (Il2CppObject*)addr, fn, nullptr, fStatic, ok);
                    if (ok) { if (!firstF) j << ","; firstF = false; j << "{\"name\":\"" << jsEsc(fn) << "\",\"type\":\"" << ft << "\",\"val\":" << vs << ",\"isProp\":false,\"canWrite\":true,\"static\":" << (fStatic?"true":"false") << "}"; }
                } catch(...) {}
            }

            for (auto& p : cur.GetProperties(false)) {
                try {
                    auto* pi = p._data;
                    if (!pi || !pi->get) continue;
                    bool pStatic = (pi->get->flags & 0x10) != 0;
                    if (!pStatic && !addr) continue;
                    std::string pn = pi->name;
                    bool dup = false; for (auto& s : seen) if (s==pn) dup=true;
                    if (dup) continue; seen.push_back(pn);
                    std::string pt = typeName(pi->get->return_type);
                    bool ok = false;
                    MethodBase getter(pi->get);
                    std::string vs = readField(pt, cur, pStatic ? nullptr : (Il2CppObject*)addr, pn, &getter, pStatic, ok);
                    if (ok) { if (!firstF) j << ","; firstF = false; j << "{\"name\":\"" << jsEsc(pn) << "\",\"type\":\"" << pt << "\",\"val\":" << vs << ",\"isProp\":true,\"canWrite\":" << (pi->set?"true":"false") << ",\"static\":" << (pStatic?"true":"false") << "}"; }
                } catch(...) {}
            }
        }

        j << "],\"methods\":[";
        bool firstM = true;
        for (Class cur = startCls; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
            auto* cn_ = cur.GetClass()->name;
            if (!cn_) break;
            if (strcmp(cn_, "Object") == 0 && cur.GetClass()->namespaze && strcmp(cur.GetClass()->namespaze, "System") == 0) break;
            for (auto& m : cur.GetMethods(false)) {
                auto* mi = m.GetInfo();
                if (!m.IsValid() || !mi || !mi->name) continue;
                if (!firstM) j << ","; firstM = false;
                j << "{\"name\":\"" << jsEsc(mi->name) << "\",\"ret\":\"" << jsEsc(typeName(mi->return_type)) << "\",\"s\":" << ((mi->flags&0x10)?"true":"false") << ",\"params\":[";
                for (int i = 0; i < (int)mi->parameters_count; i++) {
                    if (i>0) j<<",";
                    j << "{\"n\":\"arg" << i << "\",\"t\":\"" << jsEsc(mi->parameters&&mi->parameters[i]?typeName(mi->parameters[i]):"?") << "\"}";
                }
                j << "]}";
            }
        }
        j << "]}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/scene/update",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        if (!addr) { res.set_content("{\"ok\":false}", "application/json"); return; }
        std::string type = jsonVal(req.body, "type"), prop = jsonVal(req.body, "prop"), val = jsonVal(req.body, "val");
        try {
            if (type == "gameobject") {
                if (prop == "active") {
                    bool newActive = val == "true";
                    Il2CppObject* go = resolveComponentByAddr(addr);
                    if (go) {
                        Method<void> setActive(Class("UnityEngine","GameObject").GetMethod("SetActive", 1));
                        if (setActive.IsValid()) setActive[go](newActive);
                    }
                } else if (prop == "name") {
                    Il2CppObject* go = resolveComponentByAddr(addr);
                    if (go) {
                        Method<void> m(Class("UnityEngine","Object").GetMethod("set_name",1));
                        if(m.IsValid()) m[go](CreateMonoString(val));
                    }
                }
            } else if (type == "transform") {
                float x=0,y=0,z=0; sscanf(val.c_str(),"[%f,%f,%f]",&x,&y,&z); Vector3 v={x,y,z};
                Il2CppObject* tr = resolveComponentByAddr(addr);
                if (tr) {
                    if (prop=="p") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localPosition",1)); if(m.IsValid()) m[tr](v); }
                    else if (prop=="r") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localEulerAngles",1)); if(m.IsValid()) m[tr](v); }
                    else if (prop=="s") { Method<void> m(Class("UnityEngine","Transform").GetMethod("set_localScale",1)); if(m.IsValid()) m[tr](v); }
                }
            } else if (type == "script") {
                std::string name = jsonVal(req.body, "prop2");
                std::string ft   = jsonVal(req.body, "ftype");
                bool isProp      = jsonVal(req.body, "isProp") == "true";
                if (prop == "enabled") {
                    try {
                        bool newEnabled = val == "true";
                        Il2CppObject* comp = resolveComponentByAddr(addr);
                        if (comp && comp->klass) {
                            auto setEnabledOnComp = [&](Il2CppObject* c, bool v) {
                                Class compKlass(c->klass);
                                for (Class cur = compKlass; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                                    auto* cn_ = cur.GetClass()->name;
                                    if (!cn_ || strcmp(cn_, "Component") == 0 || strcmp(cn_, "Object") == 0) break;
                                    auto ep = cur.GetProperty("enabled");
                                    if (ep.IsValid() && ep._data && ep._data->set) {
                                        Method<void> m(ep._data->set); m.SetInstance(c); m(v); return true;
                                    }
                                }
                                return false;
                            };
                            auto getEnabledFromComp = [&](Il2CppObject* c) -> bool {
                                Class compKlass(c->klass);
                                for (Class cur = compKlass; cur.IsValid() && cur.GetClass(); cur = cur.GetParent()) {
                                    auto* cn_ = cur.GetClass()->name;
                                    if (!cn_ || strcmp(cn_, "Component") == 0 || strcmp(cn_, "Object") == 0) break;
                                    auto ep = cur.GetProperty("enabled");
                                    if (ep.IsValid() && ep._data && ep._data->get) {
                                        Method<bool> m(ep._data->get); m.SetInstance(c);
                                        try { return m(); } catch(...) {}
                                    }
                                }
                                return false;
                            };
                            if (newEnabled) {
                                bool already = getEnabledFromComp(comp);
                                if (!already) {
                                    setEnabledOnComp(comp, true);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                    comp = resolveComponentByAddr(addr);
                                    if (!comp) { res.set_content("{\"ok\":true,\"warn\":\"enabled but object moved\"}", "application/json"); return; }
                                }
                            } else {
                                setEnabledOnComp(comp, false);
                            }
                        }
                    } catch(...) {}
                } else {
                    std::string compNs  = jsonVal(req.body, "compNs");
                    std::string compCls = jsonVal(req.body, "compCls");
                    std::string compAsm = jsonVal(req.body, "compAsm");
                    Il2CppObject* obj = resolveComponentByAddr(addr);
                    if (!obj) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
                    Class startCls = (obj->klass) ? Class(obj->klass) :
                                     Class(compNs.c_str(), compCls.c_str(), compAsm.empty() ? Image() : Image(compAsm.c_str()));
                    for (Class cur = startCls; cur.IsValid(); cur = cur.GetParent()) {
                        if (isProp) {
                            auto p = cur.GetProperty(name.c_str());
                            if (p.IsValid() && p._data && p._data->set) { MethodBase s(p._data->set); writeField(ft, cur, obj, name, &s, val); break; }
                        } else {
                            auto f = cur.GetField(name.c_str());
                            if (f.IsValid()) { writeField(ft, cur, obj, name, nullptr, val); break; }
                        }
                    }
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/instance/update",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        std::string a=jsonVal(req.body,"asm"), ns=jsonVal(req.body,"ns"), cn=jsonVal(req.body,"cls");
        std::string name=jsonVal(req.body,"name"), ft=jsonVal(req.body,"ftype"), val=jsonVal(req.body,"val");
        bool isProp = jsonVal(req.body,"isProp") == "true";
        bool isStaticField = jsonVal(req.body,"isStatic") == "true";
        Il2CppObject* obj = isStaticField ? nullptr : resolveComponentByAddr(addr);
        if (obj || isStaticField) {
            try {
                for (Class cur(ns.c_str(),cn.c_str(),Image(a.c_str())); cur.IsValid(); cur = cur.GetParent()) {
                    if (isProp) {
                        auto p = cur.GetProperty(name.c_str());
                        if (p.IsValid() && p._data && p._data->set) { MethodBase s(p._data->set); writeField(ft,cur,obj,name,&s,val,isStaticField); break; }
                    } else {
                        auto f = cur.GetField(name.c_str());
                        if (f.IsValid()) { writeField(ft,cur,obj,name,nullptr,val,isStaticField); break; }
                    }
                }
            } catch(...) {}
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/delete",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        auto s = jsonVal(req.body, "addr");
        if (!s.empty()) {
            uintptr_t addr = (uintptr_t)strtoull(s.c_str(), nullptr, 16);
            Il2CppObject* obj = resolveComponentByAddr(addr);
            if (obj) {
                try { Method<void> m(Class("UnityEngine","Object").GetMethod("Destroy",1)); if(m.IsValid()) m(obj); } catch(...) {}
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/invoke",[](const httplib::Request& req, httplib::Response& res) {
        auto r = invokeMethod(req.body);
        std::ostringstream j;
        j << "{\"ok\":" << (r.ok?"true":"false") << ",\"value\":\"" << jsEsc(r.val) << "\",\"error\":\"" << jsEsc(r.err) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/loop/add",[](const httplib::Request& req, httplib::Response& res) {
        auto e = std::make_shared<LoopEntry>();
        e->addr      = jsonVal(req.body, "addr");
        e->asm_      = jsonVal(req.body, "asm");
        e->ns        = jsonVal(req.body, "ns");
        e->cls       = jsonVal(req.body, "cls");
        e->name      = jsonVal(req.body, "name");
        e->ftype     = jsonVal(req.body, "ftype");
        e->isProp    = jsonVal(req.body, "isProp") == "true";
        e->val       = jsonVal(req.body, "val");
        int ms       = 0;
        try { ms = std::stoi(jsonVal(req.body, "interval")); } catch(...) {}
        if (ms < 50) ms = 100;
        e->intervalMs = ms;

        std::string id = e->addr + "_" + e->cls + "_" + e->name;
        {
            std::lock_guard<std::mutex> lk(g_loopMtx);
            auto it = g_loops.find(id);
            if (it != g_loops.end()) it->second->active = false;
            g_loops[id] = e;
        }

        std::thread([e, id]() {
            AttachThread();
            while (e->active) {
                applyLoopEntry(e);
                std::this_thread::sleep_for(std::chrono::milliseconds(e->intervalMs));
            }
            std::lock_guard<std::mutex> lk(g_loopMtx);
            auto it = g_loops.find(id);
            if (it != g_loops.end() && it->second == e) g_loops.erase(it);
        }).detach();

        std::ostringstream j;
        j << "{\"ok\":true,\"id\":\"" << jsEsc(id) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/loop/remove",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_loopMtx);
        auto it = g_loops.find(id);
        if (it != g_loops.end()) { it->second->active = false; g_loops.erase(it); }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/loop/removeall",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_loopMtx);
        for (auto& kv : g_loops) kv.second->active = false;
        g_loops.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/loop/list",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(loopListJson(), "application/json");
    });

    svr.Get("/api/logs",[](const httplib::Request&, httplib::Response& res) {
        res.set_content(logsJson(), "application/json");
    });

    svr.Post("/api/logs/clear",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logBuffer.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/logs/add",[](const httplib::Request& req, httplib::Response& res) {
        std::string msg = jsonVal(req.body, "msg");
        int level = 0;
        try { level = std::stoi(jsonVal(req.body, "level")); } catch(...) {}
        addLogEntry(msg, level);
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/timescale",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        float ts = 1.0f;
        try {
            Class timeCls("UnityEngine", "Time");
            if (timeCls.IsValid()) {
                Method<float> get(timeCls.GetMethod("get_timeScale"));
                if (get.IsValid()) ts = get();
            }
        } catch(...) {}
        res.set_content("{\"value\":" + std::to_string(ts) + "}", "application/json");
    });

    svr.Post("/api/timescale",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        float val = 1.0f;
        try { val = std::stof(jsonVal(req.body, "value")); } catch(...) {}
        try {
            Class timeCls("UnityEngine", "Time");
            if (timeCls.IsValid()) {
                Method<void> set(timeCls.GetMethod("set_timeScale", 1));
                if (set.IsValid()) set(val);
            }
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/destroy",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        auto s = jsonVal(req.body, "addr");
        if (!s.empty()) {
            uintptr_t addr = (uintptr_t)strtoull(s.c_str(), nullptr, 16);
            Il2CppObject* obj = resolveComponentByAddr(addr);
            if (obj) {
                try {
                    Method<void> m(Class("UnityEngine","Object").GetMethod("Destroy",1));
                    if (m.IsValid()) m(obj);
                } catch(...) {}
            }
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/scene/create",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        std::string name = jsonVal(req.body, "name");
        if (name.empty()) name = "New GameObject";
        std::ostringstream j;
        try {
            Class goCls("UnityEngine", "GameObject");
            Method<Il2CppObject*> ctor;
            for (auto& m : goCls.GetMethods(false)) {
                auto* mi = m.GetInfo();
                if (mi && mi->name && strcmp(mi->name, ".ctor") == 0 && mi->parameters_count == 1) {
                    ctor = Method<Il2CppObject*>(m);
                    break;
                }
            }
            if (ctor.IsValid()) {
                Il2CppObject* newGO = goCls.CreateNewObjectParameters(CreateMonoString(name.c_str()));
                if (newGO) {
                    j << "{\"ok\":true,\"addr\":\"" << std::hex << (uintptr_t)newGO << "\"}";
                    res.set_content(j.str(), "application/json");
                    return;
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":false,\"error\":\"Failed to create GameObject\"}", "application/json");
    });

    svr.Post("/api/scene/addcomponent",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        std::string typeName_ = jsonVal(req.body, "type");
        if (!addr || typeName_.empty()) { res.set_content("{\"ok\":false}", "application/json"); return; }
        Il2CppObject* go = resolveComponentByAddr(addr);
        if (!go) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
        try {
            Il2CppObject* typeObj = sysTypeOf(typeName_);
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", UnityEngine.CoreModule");
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", UnityEngine");
            if (!typeObj) typeObj = sysTypeOf(typeName_ + ", Assembly-CSharp");
            if (typeObj) {
                Class goCls("UnityEngine", "GameObject");
                Method<Il2CppObject*> addComp(goCls.GetMethod("AddComponent", 1));
                if (addComp.IsValid()) {
                    Il2CppObject* comp = addComp[go](typeObj);
                    if (comp) {
                        std::ostringstream j;
                        j << "{\"ok\":true,\"addr\":\"" << std::hex << (uintptr_t)comp << "\"}";
                        res.set_content(j.str(), "application/json");
                        return;
                    }
                }
            }
        } catch(...) {}
        res.set_content("{\"ok\":false,\"error\":\"Failed to add component\"}", "application/json");
    });

    svr.Post("/api/scene/removecomponent",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        uintptr_t addr = (uintptr_t)strtoull(jsonVal(req.body, "addr").c_str(), nullptr, 16);
        if (!addr) { res.set_content("{\"ok\":false}", "application/json"); return; }
        Il2CppObject* obj = resolveComponentByAddr(addr);
        if (!obj) { res.set_content("{\"ok\":false,\"error\":\"stale address\"}", "application/json"); return; }
        try {
            Method<void> destroy(Class("UnityEngine","Object").GetMethod("Destroy",1));
            if (destroy.IsValid()) destroy(obj);
        } catch(...) {}
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/dump",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        std::ostringstream out;
        for (auto& img : Image::GetImages()) {
            auto* d = img.GetInfo();
            if (!img.IsValid() || !d || !d->name) continue;
            out << "// Assembly: " << d->name << "\n\n";
            for (auto& cls : img.GetClasses(false)) {
                auto* r = cls.GetClass();
                if (!cls.IsValid() || !r || !r->name) continue;
                std::string clsKw = "class";
                if (r->enumtype) clsKw = "enum";
                else if (r->byval_arg.valuetype) clsKw = "struct";
                else if (r->flags & 0x20) clsKw = "interface";
                if (r->namespaze && strlen(r->namespaze) > 0) {
                    out << "namespace " << r->namespaze << " {\n";
                }
                std::string indent = (r->namespaze && strlen(r->namespaze) > 0) ? "    " : "";
                std::string parent = "";
                if (r->parent && r->parent->name && strcmp(r->parent->name, "Object") != 0 && strcmp(r->parent->name, "ValueType") != 0)
                    parent = " : " + std::string(r->parent->namespaze && strlen(r->parent->namespaze) > 0 ? std::string(r->parent->namespaze) + "." : "") + r->parent->name;
                out << indent << "public " << clsKw << " " << r->name << parent << " {\n";
                for (auto& f : cls.GetFields(false)) {
                    auto* fi = f.GetInfo();
                    if (!fi || !fi->name) continue;
                    std::string tn = typeName(fi->type);
                    bool isStatic = fi->type && (fi->type->attrs & 0x10);
                    out << indent << "    // offset 0x" << std::hex << fi->offset << std::dec << "\n";
                    out << indent << "    public " << (isStatic ? "static " : "") << tn << " " << fi->name << ";\n";
                }
                for (auto& p : cls.GetProperties(false)) {
                    auto* pi = p._data;
                    if (!pi || !pi->name) continue;
                    std::string tn = pi->get && pi->get->return_type ? typeName(pi->get->return_type) : "?";
                    out << indent << "    public " << tn << " " << pi->name << " {";
                    if (pi->get) out << " get;";
                    if (pi->set) out << " set;";
                    out << " }\n";
                }
                for (auto& m : cls.GetMethods(false)) {
                    auto* mi = m.GetInfo();
                    if (!mi || !mi->name) continue;
                    char ab[32] = {};
                    if (mi->methodPointer) snprintf(ab, sizeof(ab), "0x%llX", (unsigned long long)(uintptr_t)mi->methodPointer);
                    bool isStatic = mi->flags & 0x10;
                    out << indent << "    // " << ab << "\n";
                    out << indent << "    public " << (isStatic ? "static " : "") << typeName(mi->return_type) << " " << mi->name << "(";
                    for (int i = 0; i < (int)mi->parameters_count; i++) {
                        if (i > 0) out << ", ";
                        out << (mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?") << " arg" << i;
                    }
                    out << ") {}\n";
                }
                out << indent << "}\n";
                if (r->namespaze && strlen(r->namespaze) > 0) out << "}\n";
                out << "\n";
            }
        }
        res.set_content(out.str(), "text/plain");
    });

    svr.Get("/api/scene/addable",[](const httplib::Request&, httplib::Response& res) {
        AttachThread();
        std::ostringstream j;
        j << "{";
        bool firstCat = true;
        static const struct { const char* cat; const char* ns; const char* name; } known[] = {
                {"Physics","UnityEngine","Rigidbody"},{"Physics","UnityEngine","BoxCollider"},{"Physics","UnityEngine","SphereCollider"},
                {"Physics","UnityEngine","CapsuleCollider"},{"Physics","UnityEngine","MeshCollider"},{"Physics","UnityEngine","CharacterController"},
                {"Physics","UnityEngine","WheelCollider"},{"Physics","UnityEngine","Rigidbody2D"},{"Physics","UnityEngine","BoxCollider2D"},
                {"Physics","UnityEngine","CircleCollider2D"},{"Physics","UnityEngine","PolygonCollider2D"},{"Physics","UnityEngine","CapsuleCollider2D"},
                {"Rendering","UnityEngine","MeshRenderer"},{"Rendering","UnityEngine","MeshFilter"},{"Rendering","UnityEngine","SkinnedMeshRenderer"},
                {"Rendering","UnityEngine","SpriteRenderer"},{"Rendering","UnityEngine","LineRenderer"},{"Rendering","UnityEngine","TrailRenderer"},
                {"Rendering","UnityEngine","Camera"},{"Rendering","UnityEngine","Light"},
                {"Audio","UnityEngine","AudioSource"},{"Audio","UnityEngine","AudioListener"},{"Audio","UnityEngine","AudioReverbZone"},
                {"Animation","UnityEngine","Animator"},{"Animation","UnityEngine","Animation"},{"Animation","UnityEngine","ParticleSystem"},
                {"Navigation","UnityEngine","NavMeshAgent"},{"Navigation","UnityEngine","NavMeshObstacle"},
                {"UI","UnityEngine.UI","Text"},{"UI","UnityEngine.UI","Image"},{"UI","UnityEngine.UI","Button"},
                {"UI","UnityEngine.UI","Toggle"},{"UI","UnityEngine.UI","Slider"},{"UI","UnityEngine.UI","InputField"},
                {"UI","UnityEngine.UI","Canvas"},{"UI","UnityEngine","CanvasGroup"},
                {nullptr,nullptr,nullptr}
        };
        std::map<std::string, std::vector<std::pair<std::string,std::string>>> cats;
        for (int i = 0; known[i].cat; i++) {
            Class c(known[i].ns, known[i].name);
            if (c.IsValid()) cats[known[i].cat].push_back({std::string(known[i].ns) + "." + known[i].name, known[i].name});
        }
        for (auto& img : Image::GetImages()) {
            auto* d = img.GetInfo();
            if (!img.IsValid() || !d || !d->name) continue;
            if (strstr(d->name, "Assembly-CSharp") == nullptr && strstr(d->name, "Assembly-CSharp-firstpass") == nullptr) continue;
            for (auto& cls : img.GetClasses(false)) {
                auto* r = cls.GetClass();
                if (!cls.IsValid() || !r || !r->name) continue;
                bool isMono = false;
                for (auto* cur = r; cur; cur = cur->parent)
                    if (cur->name && strcmp(cur->name, "MonoBehaviour") == 0) { isMono = true; break; }
                if (!isMono) continue;
                std::string ns = r->namespaze ? r->namespaze : "";
                std::string fullName = ns.empty() ? r->name : ns + "." + r->name;
                cats["Scripts"].push_back({fullName, r->name});
            }
        }
        for (auto& kv : cats) {
            if (!firstCat) j << ","; firstCat = false;
            j << "\"" << jsEsc(kv.first) << "\":[";
            bool first = true;
            for (auto& p : kv.second) {
                if (!first) j << ","; first = false;
                j << "{\"full\":\"" << jsEsc(p.first) << "\",\"name\":\"" << jsEsc(p.second) << "\"}";
            }
            j << "]";
        }
        j << "}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/catcher/hook",[](const httplib::Request& req, httplib::Response& res) {
        AttachThread();
        std::string asm_ = jsonVal(req.body, "asm");
        std::string ns   = jsonVal(req.body, "ns");
        std::string cls  = jsonVal(req.body, "cls");
        std::string meth = jsonVal(req.body, "method");
        std::string id   = asm_ + "|" + ns + "|" + cls + "|" + meth;
        {
            std::lock_guard<std::mutex> lk(g_catcherMtx);
            if (g_catchers.count(id)) {
                res.set_content(std::string("{\"ok\":true,\"exists\":true,\"id\":\"") + jsEsc(id) + "\"}", "application/json");
                return;
            }
        }
        int slot = -1;
        {
            std::lock_guard<std::mutex> lk(g_catcherSlotMtx);
            for (int i = 0; i < MAX_CATCHERS; i++) {
                if (!g_catcherSlots[i]) { slot = i; break; }
            }
        }
        if (slot < 0) { res.set_content("{\"ok\":false,\"error\":\"max catchers reached\"}", "application/json"); return; }
        auto entry = std::make_shared<CatcherEntry>();
        entry->id = id; entry->asm_ = asm_; entry->ns = ns; entry->cls = cls; entry->method = meth;
        entry->slotIdx = slot;
        Class c(ns.c_str(), cls.c_str(), Image(asm_.c_str()));
        if (!c.IsValid()) { res.set_content("{\"ok\":false,\"error\":\"class not found\"}", "application/json"); return; }
        MethodBase mb = c.GetMethod(meth.c_str());
        if (!mb.IsValid()) { res.set_content("{\"ok\":false,\"error\":\"method not found\"}", "application/json"); return; }
        auto* mi = mb.GetInfo();
        if (!mi || !mi->methodPointer) { res.set_content("{\"ok\":false,\"error\":\"no method pointer\"}", "application/json"); return; }
        entry->retType = typeName(mi->return_type);
        for (int i = 0; i < (int)mi->parameters_count; i++) {
            entry->paramTypes.push_back(mi->parameters && mi->parameters[i] ? typeName(mi->parameters[i]) : "?");
            entry->paramNames.push_back("arg" + std::to_string(i));
        }
        {
            std::lock_guard<std::mutex> lk(g_catcherSlotMtx);
            g_catcherSlots[slot] = new std::shared_ptr<CatcherEntry>(entry);
        }
        InvokeHook(mb, g_catcherHookFns[slot], g_catcherOrigPtrs[slot]);
        {
            std::lock_guard<std::mutex> lk(g_catcherMtx);
            g_catchers[id] = entry;
        }
        std::ostringstream j;
        j << "{\"ok\":true,\"id\":\"" << jsEsc(id) << "\"}";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/catcher/unhook",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it != g_catchers.end()) {
            it->second->active = false;
            int sl = it->second->slotIdx;
            if (sl >= 0 && sl < MAX_CATCHERS) {
                std::lock_guard<std::mutex> lk2(g_catcherSlotMtx);
                delete g_catcherSlots[sl];
                g_catcherSlots[sl] = nullptr;
                g_catcherOrigPtrs[sl] = nullptr;
            }
            g_catchers.erase(it);
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/api/catcher/clear",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = jsonVal(req.body, "id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it != g_catchers.end()) { std::lock_guard<std::mutex> lk2(it->second->callsMtx); it->second->calls.clear(); }
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/api/catcher/list",[](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& kv : g_catchers) {
            auto& e = kv.second;
            if (!first) j << ","; first = false;
            j << "{\"id\":\"" << jsEsc(e->id) << "\",\"cls\":\"" << jsEsc(e->cls) << "\",\"ns\":\"" << jsEsc(e->ns) << "\",\"method\":\"" << jsEsc(e->method) << "\",\"ret\":\"" << jsEsc(e->retType) << "\",\"active\":" << (e->active?"true":"false") << ",\"count\":";
            { std::lock_guard<std::mutex> lk2(e->callsMtx); j << e->calls.size(); }
            j << ",\"params\":[";
            for (size_t i = 0; i < e->paramTypes.size(); i++) {
                if (i>0) j<<",";
                j << "{\"t\":\"" << jsEsc(e->paramTypes[i]) << "\",\"n\":\"" << jsEsc(e->paramNames[i]) << "\"}";
            }
            j << "]}";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    svr.Get("/api/catcher/calls",[](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        std::lock_guard<std::mutex> lk(g_catcherMtx);
        auto it = g_catchers.find(id);
        if (it == g_catchers.end()) { res.set_content("[]", "application/json"); return; }
        auto& e = it->second;
        std::lock_guard<std::mutex> lk2(e->callsMtx);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& c : e->calls) {
            if (!first) j << ","; first = false;
            j << "{\"ts\":" << std::fixed << c.ts << ",\"instance\":\"" << jsEsc(c.instance) << "\",\"args\":[";
            for (size_t i = 0; i < c.args.size(); i++) {
                if (i>0) j<<",";
                j << "{\"t\":\"" << jsEsc(c.args[i].type) << "\",\"v\":" << c.args[i].val << "}";
            }
            j << "],\"ret\":\"" << jsEsc(c.ret) << "\"}";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    svr.Post("/api/lua/exec",[](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string code = jsonVal(req.body, "code");
            if (code.empty()) { res.set_content("{\"output\":\"Error: empty code\"}", "application/json"); return; }
            std::string out = executeLuaBNM(code);
            std::ostringstream j;
            j << "{\"output\":\"" << jsEsc(out) << "\"}";
            res.set_content(j.str(), "application/json");
        } catch (...) {
            res.set_content("{\"output\":\"Error: native crash in executor\"}", "application/json");
        }
    });

    svr.listen("0.0.0.0", g_port);
}

static void OnLoaded() {
    hookDebugLog();
    std::thread(startServer).detach();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, [[maybe_unused]] void* reserved) {
    JNIEnv* env;
    vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    BNM::Loading::AddOnLoadedEvent(OnLoaded);
    BNM::Loading::TryLoadByJNI(env);
    return JNI_VERSION_1_6;
}
