#pragma once

#include <QOpenGLWidget>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QElapsedTimer>
#include <QTimer>
#include <QMatrix4x4>
#include <QRandomGenerator>
#include <QDebug>

// 结构体保持 32 字节对齐
struct Particle
{
    float px,py,pz,pw; // px, py 为发射偏移量
    float vx,vy,vz,vw; // vw 为随机延迟时间 (Delay)
};

class ParticleWidget : public QOpenGLWidget,protected QOpenGLExtraFunctions
{
    Q_OBJECT

public:
    explicit ParticleWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent),ssbo(0),vao(0)
    {
        QSurfaceFormat fmt;
        fmt.setVersion(4,6);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        setFormat(fmt);

        timer.start();
        QTimer* refreshTimer = new QTimer(this);
        connect(refreshTimer,&QTimer::timeout,this,[this]()
        {
            update();
        });
        refreshTimer->start(16);
    }

    ~ParticleWidget()
    {
        makeCurrent();
        if(ssbo) glDeleteBuffers(1,&ssbo);
        if(vao) glDeleteVertexArrays(1,&vao);
        doneCurrent();
    }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();

        // 1. 必须绑定 VAO 才能在 Core Profile 下绘图
        glGenVertexArrays(1,&vao);
        glBindVertexArray(vao);

        // 2. Compute Shader (在此效果中主要作为数据容器，逻辑移至 Vertex Shader 以实现完美拖尾)
        const char* compSrc = R"(
            #version 460 core
            layout(local_size_x = 256) in;
            struct Particle { vec4 pos; vec4 vel; };
            layout(std430, binding = 0) buffer ParticleBuffer { Particle particles[]; };
            void main() { 
                // 数据由 CPU 初始化，此处保持 Buffer 活跃
            }
        )";
        computeProg.addShaderFromSourceCode(QOpenGLShader::Compute,compSrc);
        computeProg.link();

        // 3. Render Shader (核心逻辑)
        // 使用 GL_LINES，每两个顶点组成一个流星
        const char* vertSrc = R"(
            #version 460 core
            struct Particle { vec4 pos; vec4 vel; };
            layout(std430, binding = 0) buffer ParticleBuffer { Particle particles[]; };

            uniform mat4 u_MVP;
            uniform float u_Time;
            uniform float u_LoopDuration;
            uniform float u_TailLength; // 拖尾时长因子

            out float v_Alpha;

            void main() {
                uint particleIdx = gl_VertexID / 2;
                uint isTail = gl_VertexID % 2; // 0 是头，1 是尾
                
                float delay = particles[particleIdx].vel.w;
                float t = mod(u_Time + delay, u_LoopDuration);
                
                // 计算当前点的时间：如果是尾部，则取过去的时间
                float localTime = (isTail == 1) ? max(0.0, t - u_TailLength) : t;
                float progress = localTime / u_LoopDuration; // 0.0 -> 1.0

                // 穿梭逻辑：Z 轴从远处 (-100) 飞向屏幕 (0)
                float startZ = -100.0;
                float currentZ = startZ * (1.0 - progress);
                
                // 初始 XY 位置随距离轻微扩散，增加透视冲击感
                vec3 basePos = particles[particleIdx].pos.xyz;
                vec3 finalPos = vec3(basePos.xy, currentZ);

                // 头部最亮，尾部消失，且越靠近屏幕越亮
                v_Alpha = progress * (isTail == 1 ? 0.2 : 1.0);
                
                gl_Position = u_MVP * vec4(finalPos, 1.0);
            }
        )";

        const char* fragSrc = R"(
            #version 460 core
            in float v_Alpha;
            out vec4 fragColor;
           /* void main() {
                // 典型的流星蓝白色调
                vec3 color = mix(vec3(0.2, 0.4, 1.0), vec3(1.0, 1.0, 1.0), v_Alpha);
                fragColor = vec4(color, v_Alpha);
            }*/
void main() {
    // 使用 pow 增强对比度，让头部更亮，尾部消失更平滑
    float brightness = pow(v_Alpha, 1.5); 
    vec3 color = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 1.0, 1.0), brightness);
    
    // 增加一个微小的发光系数
    fragColor = vec4(color, brightness);
}
        )";

        renderProg.addShaderFromSourceCode(QOpenGLShader::Vertex,vertSrc);
        renderProg.addShaderFromSourceCode(QOpenGLShader::Fragment,fragSrc);
        renderProg.link();

        // 4. 初始化粒子 SSBO
        const int count = 4096000;
        QVector<Particle> data(count);
        auto* gen = QRandomGenerator::global();
        for(int i = 0; i < count; ++i)
        {
            // 在视野中心的平面内随机分布
            data[i].px = (float)gen->generateDouble() * 40.0f - 20.0f;
            data[i].py = (float)gen->generateDouble() * 40.0f - 20.0f;
            data[i].pz = 0.0f;
            data[i].pw = 1.0f;

            data[i].vw = (float)gen->generateDouble() * 10.0f; // 10秒内的随机启动延迟
        }

        glGenBuffers(1,&ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER,data.size() * sizeof(Particle),data.data(),GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,ssbo);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE); // 线性减淡混合，增强光线叠加感
    }

    void paintGL() override
    {
        float currentTime = timer.elapsed() / 1000.0f;

        glClearColor(0.0f,0.0f,0.02f,1.0f); // 深蓝色夜空背景
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        renderProg.bind();

        QMatrix4x4 mVP;
        mVP.perspective(60.0f,(float)width() / height(),0.1f,1000.0f);
        // 相机放在原点，观察从 -100 飞过来的粒子
        mVP.lookAt(vec3(0,0,5),vec3(0,0,0),vec3(0,1,0));

        renderProg.setUniformValue("u_MVP",mVP);
        renderProg.setUniformValue("u_Time",currentTime);
        renderProg.setUniformValue("u_LoopDuration",4.0f); // 粒子飞行总时长
        renderProg.setUniformValue("u_TailLength",0.15f);  // 拖尾长度

        glBindVertexArray(vao);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,ssbo);

        // 关键：渲染 GL_LINES，顶点数是粒子数的 2 倍
        glDrawArrays(GL_LINES,0,20000 * 2);

        renderProg.release();
    }

    void resizeGL(int w,int h) override
    {
        glViewport(0,0,w * devicePixelRatio(),h * devicePixelRatio());
    }

private:
    QOpenGLShaderProgram computeProg,renderProg;
    GLuint ssbo,vao;
    QElapsedTimer timer;
    typedef QVector3D vec3;
};