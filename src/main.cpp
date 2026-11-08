// Color Tweaks — filtro de gamma / saturacion / contraste para Geometry Dash (Geode v5)
//
// Como funciona:
//   Justo antes de que el juego haga swapBuffers (fin de cada frame), copiamos
//   el backbuffer a una textura y lo redibujamos encima con un shader que aplica
//   contraste -> saturacion -> gamma. Es una copia GPU->GPU, no lee nada a la CPU,
//   asi que no agrega frames de latencia.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <cmath>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Parametros
// ---------------------------------------------------------------------------

namespace {

struct Param {
    char const* key;
    char const* label;
    float min;
    float max;
};

constexpr Param PARAMS[3] = {
    { "gamma",      "Gamma",      0.2f, 3.0f },
    { "saturation", "Saturacion", 0.0f, 3.0f },
    { "contrast",   "Contraste",  0.0f, 3.0f },
};

float getParam(int i) {
    return static_cast<float>(Mod::get()->getSettingValue<double>(PARAMS[i].key));
}

void setParam(int i, float value) {
    Mod::get()->setSettingValue<double>(PARAMS[i].key, static_cast<double>(value));
}

bool isFilterEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

void setFilterEnabled(bool on) {
    Mod::get()->setSettingValue<bool>("enabled", on);
}

// ---------------------------------------------------------------------------
// Shader
// ---------------------------------------------------------------------------

char const* VERT_SRC = R"(
attribute vec4 a_position;
varying vec2 v_uv;
void main() {
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_uv = a_position.xy * 0.5 + 0.5;
}
)";

char const* FRAG_SRC = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_gamma;
uniform float u_saturation;
uniform float u_contrast;

void main() {
    vec3 c = texture2D(u_tex, v_uv).rgb;

    // Contraste (pivote en gris medio)
    c = (c - 0.5) * u_contrast + 0.5;

    // Saturacion (luminancia Rec.709)
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, u_saturation);

    // Gamma (>1 aclara los tonos medios, <1 los oscurece)
    c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / u_gamma));

    gl_FragColor = vec4(c, 1.0);
}
)";

struct FilterState {
    GLuint program = 0;
    GLuint texture = 0;
    int texW = 0;
    int texH = 0;
    GLint uTex = -1;
    GLint uGamma = -1;
    GLint uSaturation = -1;
    GLint uContrast = -1;
    bool broken = false; // si el shader no compila, no lo reintentamos cada frame
};

FilterState g_filter;

GLuint compileShader(GLenum type, char const* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024] = {};
        glGetShaderInfoLog(shader, sizeof(buf), nullptr, buf);
        log::error("Error compilando shader: {}", buf);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool buildProgram() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, kCCVertexAttrib_Position, "a_position");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024] = {};
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        log::error("Error enlazando shader: {}", buf);
        glDeleteProgram(prog);
        return false;
    }

    g_filter.program     = prog;
    g_filter.uTex        = glGetUniformLocation(prog, "u_tex");
    g_filter.uGamma      = glGetUniformLocation(prog, "u_gamma");
    g_filter.uSaturation = glGetUniformLocation(prog, "u_saturation");
    g_filter.uContrast   = glGetUniformLocation(prog, "u_contrast");
    return true;
}

void ensureTexture(int w, int h) {
    bool valid = g_filter.texture && glIsTexture(g_filter.texture);
    if (valid && w == g_filter.texW && h == g_filter.texH) {
        ccGLBindTexture2D(g_filter.texture);
        return;
    }
    if (valid) {
        ccGLDeleteTexture(g_filter.texture);
    }

    glGenTextures(1, &g_filter.texture);
    ccGLBindTexture2D(g_filter.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g_filter.texW = w;
    g_filter.texH = h;
}

bool nearOne(float v) {
    return std::fabs(v - 1.f) < 0.001f;
}

void applyFilter() {
    if (g_filter.broken || !isFilterEnabled()) return;

    float gamma      = getParam(0);
    float saturation = getParam(1);
    float contrast   = getParam(2);

    // Todo en 1.0 = sin cambio, nos ahorramos el pase
    if (nearOne(gamma) && nearOne(saturation) && nearOne(contrast)) return;

    // Primera vez, o el contexto GL se recreo (p.ej. al cambiar a pantalla completa)
    if (!g_filter.program || !glIsProgram(g_filter.program)) {
        g_filter.program = 0;
        g_filter.texture = 0;
        g_filter.texW = g_filter.texH = 0;
        if (!buildProgram()) {
            g_filter.broken = true;
            return;
        }
    }

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;

    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean blend   = glIsEnabled(GL_BLEND);
    GLboolean depth   = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    // Copiar el frame ya dibujado a nuestra textura (GPU -> GPU)
    ensureTexture(vp[2], vp[3]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vp[0], vp[1], vp[2], vp[3]);

    // Redibujarlo con el shader
    ccGLUseProgram(g_filter.program);
    glUniform1i(g_filter.uTex, 0);
    glUniform1f(g_filter.uGamma, gamma);
    glUniform1f(g_filter.uSaturation, saturation);
    glUniform1f(g_filter.uContrast, contrast);

    static GLfloat const quad[] = {
        -1.f, -1.f,
         1.f, -1.f,
        -1.f,  1.f,
         1.f,  1.f,
    };
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (blend)   glEnable(GL_BLEND);
    if (depth)   glEnable(GL_DEPTH_TEST);
}

} // namespace

// ---------------------------------------------------------------------------
// Hook de fin de frame
// ---------------------------------------------------------------------------

class $modify(CCEGLView) {
    void swapBuffers() {
        applyFilter();
        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// Popup con el toggle y los tres sliders
// ---------------------------------------------------------------------------

class ColorTweaksPopup : public Popup {
protected:
    Slider* m_sliders[3] = {};
    CCLabelBMFont* m_valueLabels[3] = {};
    CCMenuItemToggler* m_toggler = nullptr;

    bool init() {
        if (!Popup::init(340.f, 260.f)) return false;

        this->setTitle("Color Tweaks");
        auto size = m_mainLayer->getContentSize();

        // --- Toggle activar/desactivar ---
        m_toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(ColorTweaksPopup::onToggle), 0.8f
        );
        m_toggler->toggle(isFilterEnabled());
        m_toggler->setPosition({ size.width / 2 - 55.f, size.height - 62.f });
        m_buttonMenu->addChild(m_toggler);

        auto toggleLabel = CCLabelBMFont::create("Activado", "bigFont.fnt");
        toggleLabel->setScale(0.55f);
        toggleLabel->setAnchorPoint({ 0.f, 0.5f });
        toggleLabel->setPosition({ size.width / 2 - 35.f, size.height - 62.f });
        m_mainLayer->addChild(toggleLabel);

        // --- Sliders ---
        for (int i = 0; i < 3; i++) {
            float y = size.height - 108.f - i * 42.f;

            auto name = CCLabelBMFont::create(PARAMS[i].label, "bigFont.fnt");
            name->setScale(0.4f);
            name->setAnchorPoint({ 0.f, 0.5f });
            name->setPosition({ 18.f, y });
            m_mainLayer->addChild(name);

            auto slider = Slider::create(this, menu_selector(ColorTweaksPopup::onSlider), 0.6f);
            slider->setPosition({ 195.f, y });
            slider->getThumb()->setTag(i);
            m_mainLayer->addChild(slider);
            m_sliders[i] = slider;

            auto value = CCLabelBMFont::create("", "bigFont.fnt");
            value->setScale(0.4f);
            value->setAnchorPoint({ 1.f, 0.5f });
            value->setPosition({ size.width - 16.f, y });
            m_mainLayer->addChild(value);
            m_valueLabels[i] = value;
        }

        // --- Boton reset ---
        auto resetSpr = ButtonSprite::create("Reset", "goldFont.fnt", "GJ_button_04.png", 0.7f);
        auto resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(ColorTweaksPopup::onReset)
        );
        resetBtn->setPosition({ size.width / 2, 26.f });
        m_buttonMenu->addChild(resetBtn);

        this->refreshUI();
        return true;
    }

    void refreshUI() {
        for (int i = 0; i < 3; i++) {
            float v = getParam(i);
            auto& p = PARAMS[i];
            m_sliders[i]->setValue((v - p.min) / (p.max - p.min));
            m_valueLabels[i]->setString(fmt::format("{:.2f}", v).c_str());
        }
    }

    void onSlider(CCObject* sender) {
        auto thumb = static_cast<SliderThumb*>(sender);
        int i = thumb->getTag();
        if (i < 0 || i > 2) return;

        auto& p = PARAMS[i];
        float v = p.min + thumb->getValue() * (p.max - p.min);
        v = std::round(v * 100.f) / 100.f;

        setParam(i, v);
        m_valueLabels[i]->setString(fmt::format("{:.2f}", v).c_str());
    }

    void onToggle(CCObject* sender) {
        // GD cambia el estado del toggler DESPUES del callback
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        setFilterEnabled(!toggler->isToggled());
    }

    void onReset(CCObject*) {
        for (int i = 0; i < 3; i++) setParam(i, 1.f);
        this->refreshUI();
    }

public:
    static ColorTweaksPopup* create() {
        auto ret = new ColorTweaksPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Boton en el menu de pausa
// ---------------------------------------------------------------------------

class $modify(CTPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;

        auto label = CCLabelBMFont::create("FX", "bigFont.fnt");
        label->setScale(0.6f);
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green, CircleBaseSize::Small);

        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(CTPauseLayer::onColorTweaks)
        );
        btn->setID("color-tweaks-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onColorTweaks(CCObject*) {
        if (auto popup = ColorTweaksPopup::create()) popup->show();
    }
};
