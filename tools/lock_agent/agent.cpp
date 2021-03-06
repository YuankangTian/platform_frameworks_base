/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

#include <jni.h>

#include <jvmti.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <sys/stat.h>

// We need dladdr.
#if !defined(__APPLE__) && !defined(_WIN32)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#define DEFINED_GNU_SOURCE
#endif
#include <dlfcn.h>
#ifdef DEFINED_GNU_SOURCE
#undef _GNU_SOURCE
#undef DEFINED_GNU_SOURCE
#endif
#endif

// Slicer's headers have code that triggers these warnings. b/65298177
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"

#include <slicer/dex_ir.h>
#include <slicer/code_ir.h>
#include <slicer/dex_bytecode.h>
#include <slicer/dex_ir_builder.h>
#include <slicer/writer.h>
#include <slicer/reader.h>

#pragma clang diagnostic pop

namespace {

JavaVM* gJavaVM = nullptr;

// Converts a class name to a type descriptor
// (ex. "java.lang.String" to "Ljava/lang/String;")
std::string classNameToDescriptor(const char* className) {
    std::stringstream ss;
    ss << "L";
    for (auto p = className; *p != '\0'; ++p) {
        ss << (*p == '.' ? '/' : *p);
    }
    ss << ";";
    return ss.str();
}

using namespace dex;
using namespace lir;

bool transform(std::shared_ptr<ir::DexFile> dexIr) {
    bool modified = false;

    std::unique_ptr<ir::Builder> builder;

    for (auto& method : dexIr->encoded_methods) {
        // Do not look into abstract/bridge/native/synthetic methods.
        if ((method->access_flags & (kAccAbstract | kAccBridge | kAccNative | kAccSynthetic))
                != 0) {
            continue;
        }

        struct HookVisitor: public Visitor {
            HookVisitor(std::unique_ptr<ir::Builder>* b, std::shared_ptr<ir::DexFile> d_ir,
                    CodeIr* c_ir) :
                    b(b), dIr(d_ir), cIr(c_ir) {
            }

            bool Visit(Bytecode* bytecode) override {
                if (bytecode->opcode == OP_MONITOR_ENTER) {
                    prepare();
                    addCall(bytecode, OP_INVOKE_STATIC_RANGE, hookType, "preLock", voidType,
                            objectType, reinterpret_cast<VReg*>(bytecode->operands[0])->reg);
                    myModified = true;
                    return true;
                }
                if (bytecode->opcode == OP_MONITOR_EXIT) {
                    prepare();
                    addCall(bytecode, OP_INVOKE_STATIC_RANGE, hookType, "postLock", voidType,
                            objectType, reinterpret_cast<VReg*>(bytecode->operands[0])->reg);
                    myModified = true;
                    return true;
                }
                return false;
            }

            void prepare() {
                if (*b == nullptr) {
                    *b = std::unique_ptr<ir::Builder>(new ir::Builder(dIr));
                }
                if (voidType == nullptr) {
                    voidType = (*b)->GetType("V");
                    hookType = (*b)->GetType("Lcom/android/lock_checker/LockHook;");
                    objectType = (*b)->GetType("Ljava/lang/Object;");
                }
            }

            void addInst(lir::Instruction* instructionAfter, Opcode opcode,
                    const std::list<Operand*>& operands) {
                auto instruction = cIr->Alloc<Bytecode>();

                instruction->opcode = opcode;

                for (auto it = operands.begin(); it != operands.end(); it++) {
                    instruction->operands.push_back(*it);
                }

                cIr->instructions.InsertBefore(instructionAfter, instruction);
            }

            void addCall(lir::Instruction* instructionAfter, Opcode opcode, ir::Type* type,
                    const char* methodName, ir::Type* returnType,
                    const std::vector<ir::Type*>& types, const std::list<int>& regs) {
                auto proto = (*b)->GetProto(returnType, (*b)->GetTypeList(types));
                auto method = (*b)->GetMethodDecl((*b)->GetAsciiString(methodName), proto, type);

                VRegList* paramRegs = cIr->Alloc<VRegList>();
                for (auto it = regs.begin(); it != regs.end(); it++) {
                    paramRegs->registers.push_back(*it);
                }

                addInst(instructionAfter, opcode,
                        { paramRegs, cIr->Alloc<Method>(method, method->orig_index) });
            }

            void addCall(lir::Instruction* instructionAfter, Opcode opcode, ir::Type* type,
                    const char* methodName, ir::Type* returnType, ir::Type* paramType,
                    u4 paramVReg) {
                auto proto = (*b)->GetProto(returnType, (*b)->GetTypeList( { paramType }));
                auto method = (*b)->GetMethodDecl((*b)->GetAsciiString(methodName), proto, type);

                VRegRange* args = cIr->Alloc<VRegRange>(paramVReg, 1);

                addInst(instructionAfter, opcode,
                        { args, cIr->Alloc<Method>(method, method->orig_index) });
            }

            std::unique_ptr<ir::Builder>* b;
            std::shared_ptr<ir::DexFile> dIr;
            CodeIr* cIr;
            ir::Type* voidType = nullptr;
            ir::Type* hookType = nullptr;
            ir::Type* objectType = nullptr;
            bool myModified = false;
        };

        CodeIr c(method.get(), dexIr);
        HookVisitor visitor(&builder, dexIr, &c);

        for (auto it = c.instructions.begin(); it != c.instructions.end(); ++it) {
            lir::Instruction* fi = *it;
            fi->Accept(&visitor);
        }

        if (visitor.myModified) {
            modified = true;
            c.Assemble();
        }
    }

    return modified;
}

std::pair<dex::u1*, size_t> maybeTransform(const char* name, size_t classDataLen,
        const unsigned char* classData, dex::Writer::Allocator* allocator) {
    // Isolate byte code of class class. This is needed as Android usually gives us more
    // than the class we need.
    dex::Reader reader(classData, classDataLen);

    dex::u4 index = reader.FindClassIndex(classNameToDescriptor(name).c_str());
    CHECK_NE(index, kNoIndex);
    reader.CreateClassIr(index);
    std::shared_ptr<ir::DexFile> ir = reader.GetIr();

    if (!transform(ir)) {
        return std::make_pair(nullptr, 0);
    }

    size_t new_size;
    dex::Writer writer(ir);
    dex::u1* newClassData = writer.CreateImage(allocator, &new_size);
    return std::make_pair(newClassData, new_size);
}

void transformHook(jvmtiEnv* jvmtiEnv, JNIEnv* env ATTRIBUTE_UNUSED,
        jclass classBeingRedefined ATTRIBUTE_UNUSED, jobject loader, const char* name,
        jobject protectionDomain ATTRIBUTE_UNUSED, jint classDataLen,
        const unsigned char* classData, jint* newClassDataLen, unsigned char** newClassData) {
    // Even reading the classData array is expensive as the data is only generated when the
    // memory is touched. Hence call JvmtiAgent#shouldTransform to check if we need to transform
    // the class.

    // Skip bootclasspath classes. TODO: Make this configurable.
    if (loader == nullptr) {
        return;
    }

    // Do not look into java.* classes. Should technically be filtered by above, but when that's
    // configurable have this.
    if (strncmp("java", name, 4) == 0) {
        return;
    }

    // Do not look into our Java classes.
    if (strncmp("com/android/lock_checker", name, 24) == 0) {
        return;
    }

    class JvmtiAllocator: public dex::Writer::Allocator {
    public:
        explicit JvmtiAllocator(::jvmtiEnv* jvmti) :
                jvmti_(jvmti) {
        }

        void* Allocate(size_t size) override {
            unsigned char* res = nullptr;
            jvmti_->Allocate(size, &res);
            return res;
        }

        void Free(void* ptr) override {
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(ptr));
        }

    private:
        ::jvmtiEnv* jvmti_;
    };
    JvmtiAllocator allocator(jvmtiEnv);
    std::pair<dex::u1*, size_t> result = maybeTransform(name, classDataLen, classData,
            &allocator);

    if (result.second > 0) {
        *newClassData = result.first;
        *newClassDataLen = static_cast<jint>(result.second);
    }
}

void dataDumpRequestHook(jvmtiEnv* jvmtiEnv ATTRIBUTE_UNUSED) {
    if (gJavaVM == nullptr) {
        LOG(ERROR) << "No JavaVM for dump";
        return;
    }
    JNIEnv* env;
    if (gJavaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOG(ERROR) << "Could not get env for dump";
        return;
    }
    jclass lockHookClass = env->FindClass("com/android/lock_checker/LockHook");
    if (lockHookClass == nullptr) {
        env->ExceptionClear();
        LOG(ERROR) << "Could not find LockHook class";
        return;
    }
    jmethodID dumpId = env->GetStaticMethodID(lockHookClass, "dump", "()V");
    if (dumpId == nullptr) {
        env->ExceptionClear();
        LOG(ERROR) << "Could not find LockHook.dump";
        return;
    }
    env->CallStaticVoidMethod(lockHookClass, dumpId);
    env->ExceptionClear();
}

// A function for dladdr to search.
extern "C" __attribute__ ((visibility ("default"))) void lock_agent_tag_fn() {
}

bool fileExists(const std::string& path) {
    struct stat statBuf;
    int rc = stat(path.c_str(), &statBuf);
    return rc == 0;
}

std::string findLockAgentJar() {
    // Check whether the jar is located next to the agent's so.
#ifndef __APPLE__
    {
        Dl_info info;
        if (dladdr(reinterpret_cast<const void*>(&lock_agent_tag_fn), /* out */ &info) != 0) {
            std::string lockAgentSoPath = info.dli_fname;
            std::string dir = android::base::Dirname(lockAgentSoPath);
            std::string lockAgentJarPath = dir + "/" + "lockagent.jar";
            if (fileExists(lockAgentJarPath)) {
                return lockAgentJarPath;
            }
        } else {
            LOG(ERROR) << "dladdr failed";
        }
    }
#endif

    std::string sysFrameworkPath = "/system/framework/lockagent.jar";
    if (fileExists(sysFrameworkPath)) {
        return sysFrameworkPath;
    }

    std::string relPath = "lockagent.jar";
    if (fileExists(relPath)) {
        return relPath;
    }

    return "";
}

void prepareHook(jvmtiEnv* env) {
    // Inject the agent Java code.
    {
        std::string path = findLockAgentJar();
        if (path.empty()) {
            LOG(FATAL) << "Could not find lockagent.jar";
        }
        LOG(INFO) << "Will load Java parts from " << path;
        jvmtiError res = env->AddToBootstrapClassLoaderSearch(path.c_str());
        if (res != JVMTI_ERROR_NONE) {
            LOG(FATAL) << "Could not add lockagent from " << path << " to boot classpath: " << res;
        }
    }

    jvmtiCapabilities caps;
    memset(&caps, 0, sizeof(caps));
    caps.can_retransform_classes = 1;

    if (env->AddCapabilities(&caps) != JVMTI_ERROR_NONE) {
        LOG(FATAL) << "Could not add caps";
    }

    jvmtiEventCallbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.ClassFileLoadHook = transformHook;
    cb.DataDumpRequest = dataDumpRequestHook;

    if (env->SetEventCallbacks(&cb, sizeof(cb)) != JVMTI_ERROR_NONE) {
        LOG(FATAL) << "Could not set cb";
    }

    if (env->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr)
            != JVMTI_ERROR_NONE) {
        LOG(FATAL) << "Could not enable events";
    }
    if (env->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DATA_DUMP_REQUEST, nullptr)
            != JVMTI_ERROR_NONE) {
        LOG(FATAL) << "Could not enable events";
    }
}

jint attach(JavaVM* vm, char* options ATTRIBUTE_UNUSED, void* reserved ATTRIBUTE_UNUSED) {
    gJavaVM = vm;

    jvmtiEnv* env;
    jint jvmError = vm->GetEnv(reinterpret_cast<void**>(&env), JVMTI_VERSION_1_2);
    if (jvmError != JNI_OK) {
        return jvmError;
    }

    prepareHook(env);

    return JVMTI_ERROR_NONE;
}

extern "C" JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    return attach(vm, options, reserved);
}

extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    return attach(vm, options, reserved);
}

int locktest_main(int argc, char *argv[]) {
    if (argc != 3) {
        LOG(FATAL) << "Need two arguments: dex-file class-name";
    }
    struct stat statBuf;
    int rc = stat(argv[1], &statBuf);
    if (rc != 0) {
        PLOG(FATAL) << "Could not get file size for " << argv[1];
    }
    std::unique_ptr<char[]> data(new char[statBuf.st_size]);
    {
        android::base::unique_fd fd(open(argv[1], O_RDONLY));
        if (fd.get() == -1) {
            PLOG(FATAL) << "Could not open file " << argv[1];
        }
        if (!android::base::ReadFully(fd.get(), data.get(), statBuf.st_size)) {
            PLOG(FATAL) << "Could not read file " << argv[1];
        }
    }

    class NewDeleteAllocator: public dex::Writer::Allocator {
    public:
        explicit NewDeleteAllocator() {
        }

        void* Allocate(size_t size) override {
            return new char[size];
        }

        void Free(void* ptr) override {
            delete[] reinterpret_cast<char*>(ptr);
        }
    };
    NewDeleteAllocator allocator;

    std::pair<dex::u1*, size_t> result = maybeTransform(argv[2], statBuf.st_size,
            reinterpret_cast<unsigned char*>(data.get()), &allocator);

    if (result.second == 0) {
        LOG(INFO) << "No transformation";
        return 0;
    }

    std::string newName(argv[1]);
    newName.append(".new");

    {
        android::base::unique_fd fd(
                open(newName.c_str(), O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR));
        if (fd.get() == -1) {
            PLOG(FATAL) << "Could not open file " << newName;
        }
        if (!android::base::WriteFully(fd.get(), result.first, result.second)) {
            PLOG(FATAL) << "Could not write file " << newName;
        }
    }
    LOG(INFO) << "Transformed file written to " << newName;

    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    return locktest_main(argc, argv);
}
