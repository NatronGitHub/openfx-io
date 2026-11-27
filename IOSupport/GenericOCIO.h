/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2021 The Natron Developers
 * (C) 2013-2018 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX GenericOCIO plugin add-on.
 * Adds OpenColorIO functionality to any plugin.
 */

#ifndef IO_GenericOCIO_h
#define IO_GenericOCIO_h

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsPixelProcessor.h"

// define OFX_OCIO_CHOICE to enable the colorspace choice popup menu
#define OFX_OCIO_CHOICE

#ifdef OFX_IO_USING_OCIO
#include <OpenColorIO/OpenColorIO.h>

#if OCIO_VERSION_HEX < 0x02000000
#error "The OpenColorIO plug-ins and readers/writers require OpenColorIO 2.0 or later"
#endif

namespace OCIO_NAMESPACE {
class OpenGLBuilder;
typedef OCIO_SHARED_PTR<OpenGLBuilder> OpenGLBuilderRcPtr;
}
#endif

#include "IOMutex.h"
#include "IOUtility.h"
#ifdef OFX_IO_USING_OCIO
#include "OCIOProcessorCache.h"
#endif

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

// The scoped enums used as bitmasks (GenericOCIO's FixColorspace and StringKeys) opt in to operator| and has() with
// template <> struct IsFlagEnum<E> : std::true_type {};
template <class E>
struct IsFlagEnum : std::false_type {};

template <class E, class = std::enable_if_t<IsFlagEnum<E>::value>>
constexpr E
operator|(E a,
          E b)
{
    using U = std::underlying_type_t<E>;

    return E(U(a) | U(b));
}

// true if set contains the flag f
template <class E, class = std::enable_if_t<IsFlagEnum<E>::value>>
constexpr bool
has(E set,
    E f)
{
    using U = std::underlying_type_t<E>;

    return (U(set) & U(f)) != 0;
}

#ifdef OFX_IO_USING_OCIO
#define kOCIOParamConfigFile "ocioConfigFile"
#define kOCIOParamConfigFileLabel "OCIO Config File", "OpenColorIO configuration file"
#define kOCIOParamInputSpace "ocioInputSpace"
#define kOCIOParamInputSpaceLabel "Input Colorspace"
#define kOCIOParamInputSpaceHint "Input data is taken to be in this colorspace."
#define kOCIOParamOutputSpace "ocioOutputSpace"
#define kOCIOParamOutputSpaceLabel "Output Colorspace"
#define kOCIOParamOutputSpaceHint "Output data is taken to be in this colorspace."
#ifdef OFX_OCIO_CHOICE
#define kOCIOParamInputSpaceChoice "ocioInputSpaceIndex"
#define kOCIOParamOutputSpaceChoice "ocioOutputSpaceIndex"
#endif
#define kOCIOHelpButton "ocioHelp"
#define kOCIOHelpLooksButton "ocioHelpLooks"
#define kOCIOHelpDisplaysButton "ocioHelpDisplays"
#define kOCIOHelpButtonLabel "OCIO config help..."
#define kOCIOHelpButtonHint "Help about the OpenColorIO configuration."
#else
#define kOCIOParamInputSpaceLabel ""
#define kOCIOParamOutputSpaceLabel ""
#endif

#define kOCIOParamInverse "ocioInverse"
#define kOCIOParamInverseLabel "Inverse"
#define kOCIOParamInverseHint "Invert the transform: convert from the output colorspace to the input colorspace."
#define kOCIOParamSwapColorspaces "ocioSwapColorspaces"
#define kOCIOParamSwapColorspacesLabel "Swap Colorspaces"
#define kOCIOParamSwapColorspacesHint "Swap the input and output colorspaces."

#define kOCIOParamContext "Context"
#define kOCIOParamContextLabel "OCIO Context"
#define kOCIOParamContextHint                                                                                             \
    "OCIO Contexts allow you to apply specific LUTs or grades to different shots.\n"                                      \
    "Here you can specify the context name (key) and its corresponding value.\n"                                          \
    "Full details of how to set up contexts and add them to your config can be found in the OpenColorIO documentation:\n" \
    "http://opencolorio.org/userguide/contexts.html"

#define kOCIOParamContextKey1 "key1"
#define kOCIOParamContextValue1 "value1"
#define kOCIOParamContextKey2 "key2"
#define kOCIOParamContextValue2 "value2"
#define kOCIOParamContextKey3 "key3"
#define kOCIOParamContextValue3 "value3"
#define kOCIOParamContextKey4 "key4"
#define kOCIOParamContextValue4 "value4"

#if defined(OFX_IO_USING_OCIO)
class OCIOOpenGLContextData {
public:
    std::string processorCacheID;
    OCIO_NAMESPACE::OpenGLBuilderRcPtr glBuilder;

public:
    OCIOOpenGLContextData();

    ~OCIOOpenGLContextData();
};
#endif

class GenericOCIO {
    friend class OCIOProcessor;

public:
    GenericOCIO(OFX::ImageEffect* parent);
    bool isIdentity(double time) const;

#if defined(OFX_IO_USING_OCIO) && defined(OFX_SUPPORTS_OPENGLRENDER)
    static void applyGL(const OFX::Texture* srcImg,
                        const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                        OCIOOpenGLContextData* contextData);
#endif

    void apply(double time, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int pixelComponentCount, int rowBytes);
    void changedParam(const OFX::InstanceChangedArgs& args, const std::string& paramName);
    void purgeCaches();
    void getInputColorspaceDefault(std::string& v) const;
    void getInputColorspace(std::string& v) const;
    void getInputColorspaceAtTime(double time, std::string& v) const;
    void getOutputColorspaceDefault(std::string& v) const;
    void getOutputColorspace(std::string& v) const;
    void getOutputColorspaceAtTime(double time, std::string& v) const;
    bool hasColorspace(const char* name) const;
    void setInputColorspace(const char* name);
    void setOutputColorspace(const char* name);
    // the colorspaces actually used for processing (input and output are swapped if the optional Inverse param is checked)
    void getProcessingColorspacesAtTime(double time, std::string& inputSpace, std::string& outputSpace) const;
    // swap the input and output colorspace params
    void swapColorspaces(double time);
    static void describeInContextInverseAndSwap(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context, OFX::PageParamDescriptor* page);
#ifdef OFX_IO_USING_OCIO
    // the context of config (the one returned by getConfig()) with the OCIO Context params at time
    OCIO_NAMESPACE::ConstContextRcPtr getLocalContext(const OCIO_NAMESPACE::ConstConfigRcPtr& config, double time) const;
    // a copy of the config, taken under its lock: a render uses its copy, which the main thread can't replace (#275)
    OCIO_NAMESPACE::ConstConfigRcPtr getConfig() const
    {
        return _config.get();
    }
    OCIO_NAMESPACE::ConstProcessorRcPtr getOrCreateProcessor(double time) const;

#endif
    bool configIsDefault() const;

    // Each of the following functions re-reads the OCIO config: Not optimal but more clear.
    static void describeInContextInput(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context, OFX::PageParamDescriptor* page, const char* inputSpaceNameDefault, const char* inputSpaceLabel = kOCIOParamInputSpaceLabel);
    static void describeInContextOutput(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context, OFX::PageParamDescriptor* page, const char* outputSpaceNameDefault, const char* outputSpaceLabel = kOCIOParamOutputSpaceLabel);
    static void describeInContextContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context, OFX::PageParamDescriptor* page);

    // update the choice menus (and their state) from the string params, at time
    void refreshInputAndOutputState(double time);

    // While an object of this class exists, the effect is in an instance changed action (where OFX 1.4 allows
    // setting keys, see paramSetValueAtTime in ofxParam.h): GenericOCIO may then set keys on the colorspace params.
    // changedParam() and swapColorspaces() create one; an effect that calls refreshInputAndOutputState(),
    // setInputColorspace() or setOutputColorspace() from its own changedParam() must create one there too (outside of
    // an instance changed action, e.g. in createInstance, GenericOCIO only sets values).
    class InstanceChanged {
    public:
        explicit InstanceChanged(GenericOCIO* ocio)
            : _ocio(ocio)
        {
            if (_ocio->_instanceChangedDepth++ == 0) {
                // a new action: the params may have other expressions
                _ocio->resetExpressionCache();
            }
        }
        ~InstanceChanged() { --_ocio->_instanceChangedDepth; }
        InstanceChanged(const InstanceChanged&) = delete;
        InstanceChanged& operator=(const InstanceChanged&) = delete;

    private:
        GenericOCIO* _ocio;
    };

    // true while GenericOCIO sets the colorspace params: an instance changed action of one of them (Natron reports a
    // change of a param by a plug-in before the plug-in's call returns, a deleted key as a user edit) is then not a
    // user edit
    bool isSettingColorspaces() const
    {
#ifdef OFX_IO_USING_OCIO
        return _settingColorspaces;
#else
        return false;
#endif
    }

#ifdef OFX_IO_USING_OCIO
    // true if args is a user edit of the input (or output) colorspace param or of its menu, not a change that
    // GenericOCIO makes (see isSettingColorspaces(): e.g. Natron reports a key that GenericOCIO deletes as a user edit)
    bool isUserInputColorspaceEdit(const OFX::InstanceChangedArgs& args, const std::string& paramName) const;
    bool isUserOutputColorspaceEdit(const OFX::InstanceChangedArgs& args, const std::string& paramName) const;
#endif

public:
    typedef IOMutex Mutex;
    typedef IOAutoMutex AutoMutex;

private:
    void loadConfig();
    void resetExpressionCache();
#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
    struct Side;
    // what fixColorspaceAtTime() does (a bitmask)
    enum class FixColorspace : unsigned {
        canonicalize = 1, //< give the string param the canonical name of its colorspace
        fallback = 2, //< give it a valid colorspace if its colorspace is not in the config
        warn = 4, //< with fallback: warn about the unknown colorspace
    };
    // canonicalize the colorspace string param of a side that changed at time, or reset it to a valid colorspace
    void fixColorspaceAtTime(Side& side, double time, FixColorspace what);
    // the side of a colorspace param (its string param or its menu), or NULL
    Side* sideOfParam(const std::string& paramName, bool* isChoice);
    // update the state and value (or keys) of the menu of a side from its string param
    void colorspaceCheck(Side& side, double time);
    // hasExpressionKeys() of the string param or the menu of a side, cached in an instance changed action
    bool strHasExpression(Side& side);
    bool choiceHasExpression(Side& side);
    // true if the menu of a side has the same keys as its string param (see GenericOCIO.cpp)
    bool choiceMirrorsString(Side& side);
    // give the menus the keys of the string params
    void syncChoicesFromStrings();
    // set the string param of a side from its menu, after a user edit of the menu at time
    void changedChoice(Side& side, double time);
#endif

#ifdef OFX_IO_USING_OCIO
    // While an object of this class exists, GenericOCIO sets the colorspace params, and changedParam() ignores the
    // instance changed actions of these params: in Natron, setting, or deleting keys of, a param re-enters
    // changedParam() before it returns.
    class SettingColorspaces {
    public:
        explicit SettingColorspaces(GenericOCIO* ocio)
            : _ocio(ocio)
            , _wasSetting(ocio->_settingColorspaces)
        {
            _ocio->_settingColorspaces = true;
        }
        ~SettingColorspaces() { _ocio->_settingColorspaces = _wasSetting; }
        SettingColorspaces(const SettingColorspaces&) = delete;
        SettingColorspaces& operator=(const SettingColorspaces&) = delete;

    private:
        GenericOCIO* _ocio;
        bool _wasSetting;
    };
#endif

    OFX::ImageEffect* _parent;
    bool _created;
    int _instanceChangedDepth; //< see InstanceChanged
#ifdef OFX_IO_USING_OCIO
    bool _settingColorspaces; //< see SettingColorspaces
    std::string _ocioConfigFileName;
    OFX::StringParam* _ocioConfigFile; //< filepath of the OCIO config file
    OFX::StringParam* _inputSpace;
    OFX::StringParam* _outputSpace;
    OFX::BooleanParam* _inverse; //< optional: swap input and output colorspaces when processing
#ifdef OFX_OCIO_CHOICE
    bool _choiceIsOk; //< true if the choice menu contains the right entries
    std::string _choiceFileName; //< the name of the OCIO config file that was used for the choice menu
    // The colorspace params of a side (input or output): its string param, which is rendered and saved, and its menu.
    // The menus are not saved: after loading a project, they have no keys, even if the strings have keys, and they
    // show the value set outside of an instance changed action (initialIndex): the string's colorspace at time 0, set
    // by the constructor, or the one guessed from the file name by a writer's createInstance. They get the strings'
    // keys at the first instance changed action that GenericOCIO gets, the only action where a plug-in may set keys
    // (_choiceAnimSynced, not saved either). Until then, a menu key with the value initialIndex (e.g. Set Key on the
    // menu) is not an edit (see changedChoice()).
    struct Side {
        OFX::StringParam* str; //< NULL if the effect doesn't have this side
        OFX::ChoiceParam* choice; //< its menu
        int initialIndex;
        std::optional<bool> strExpression; //< hasExpressionKeys(str), if known in this instance changed action
        std::optional<bool> choiceExpression; //< hasExpressionKeys(choice), likewise
    };
    Side _sides[2]; //< input, output
    bool _choiceAnimSynced;
#endif
    // the OCIO Context params (kOCIOParamContextKey1... and kOCIOParamContextValue1...), NULL if the effect doesn't
    // have them
    static constexpr int kContextVariables = 4;
    OFX::StringParam* _contextKeys[kContextVariables];
    OFX::StringParam* _contextValues[kContextVariables];

    // Thread safety: the render actions (and isIdentity) run on render threads, concurrently with each other and with
    // the main thread (OFX ofxThreadSafety.rst: "An instance must be able to handle simultaneous actions called on the
    // main and render threads"), whose instance changed action loads a new config (loadConfig()).
    // - _config has its own mutex (see OCIOSharedConfig). Only loadConfig() and purgeCaches(), on the main thread,
    //   replace it; the main thread may thus read it without the lock (mainConfig()). Other threads take a copy with
    //   getConfig(), and use only that copy in an action: the config it points to is const and stays alive while they
    //   hold it.
    // - _procCache has its own mutex (see OCIOProcessorCache).
    // - Neither lock is held while taking the other one, or while calling out (lock order: none).
    OCIOSharedConfig _config;
    // the config, on the main thread (see above)
    const OCIO_NAMESPACE::ConstConfigRcPtr& mainConfig() const { return _config.getOnMainThread(); }

    // everything that the processor of getOrCreateProcessor() depends on
    struct ProcKey {
        OCIOConfigContextKey configContext;
        std::string inputSpace;
        std::string outputSpace;

        friend bool operator==(const ProcKey& a, const ProcKey& b)
        {
            return std::tie(a.configContext, a.inputSpace, a.outputSpace) == std::tie(b.configContext, b.inputSpace, b.outputSpace);
        }
    };
    mutable OCIOProcessorCache<ProcKey> _procCache; // (isIdentity() and getOrCreateProcessor() are const)
    // the processor from inputSpace to outputSpace, with the context of config at time (from _procCache)
    OCIO_NAMESPACE::ConstProcessorRcPtr processorFor(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                                                     double time,
                                                     const std::string& inputSpace,
                                                     const std::string& outputSpace) const;
#endif
};

#ifdef OFX_IO_USING_OCIO
// Hold one around each call of OCIO's Processor::getOptimizedCPUProcessor(), getOptimizedGPUProcessor() and
// getOptimizedLegacyGPUProcessor(). Before OpenColorIO 2.6, they read OCIO_OPTIMIZATION_FLAGS with GetEnvVariable(),
// which returns a static std::string that every call writes (OpenColorIO 2.5.1 src/OpenColorIO/Platform.cpp:23-28,
// Processor.cpp:352): concurrent renders race on it (ThreadSanitizer, test-ocio-processor-cache-tsan). The lock
// guards only that string. OCIO takes its own locks under it and calls no plug-in code, so it can't be part of a
// lock-order cycle. It orders only the calls of this bundle (one mutex per binary): the host (e.g. Natron),
// OpenImageIO's own OCIO calls and other bundles (make nomulti builds one per plug-in group) may still race on the
// same string with OpenColorIO < 2.6. OpenColorIO 2.6 makes the string thread_local (2.6.0 Platform.cpp:25): no lock
// there.
class OCIOOptimizedProcessorLock {
public:
    OCIOOptimizedProcessorLock();
    OCIOOptimizedProcessorLock(const OCIOOptimizedProcessorLock&) = delete;
    OCIOOptimizedProcessorLock& operator=(const OCIOOptimizedProcessorLock&) = delete;

private:
#if OCIO_VERSION_HEX < 0x02060000
    std::lock_guard<std::mutex> _lock;
#endif
};

class OCIOProcessor
    : public OFX::PixelProcessor {
public:
    // ctor
    OCIOProcessor(OFX::ImageEffect& instance)
        : OFX::PixelProcessor(instance)
        , _proc()
        , _instance(&instance)
    {
    }

    // and do some processing
    void multiThreadProcessImages(const OfxRectI& procWindow, const OfxPointD& rs);

    void setProcessor(const OCIO_NAMESPACE::ConstProcessorRcPtr& proc)
    {
        _proc = proc;
    }

private:
    OCIO_NAMESPACE::ConstProcessorRcPtr _proc;
    OFX::ImageEffect* _instance;
};
#endif

// Helper class to set the C locale when doing OCIO calls.
// See https://github.com/AcademySoftwareFoundation/OpenColorIO/issues/297#issuecomment-505636123
class AutoSetAndRestoreThreadLocale {
public:
    AutoSetAndRestoreThreadLocale();
    ~AutoSetAndRestoreThreadLocale();

private:
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    std::string ssaLocale;
    int previousThreadConfig;
#else
    locale_t oldLocale;
    locale_t currentLocale;
#endif
};

#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
template <>
struct IsFlagEnum<GenericOCIO::FixColorspace> : std::true_type {};
#endif

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // ifndef IO_GenericOCIO_h
