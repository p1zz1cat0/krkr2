#include "emotefile.h"
#include "EmoteInternal.h"


#include <sstream>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <cmath>
#include <cassert>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#define GLM_ASSERT_VALID(matrix) \
    do \
    { \
        const glm::mat4& m = (matrix); \
        for (int i = 0; i < 4; ++i) \
        { \
            for (int j = 0; j < 4; ++j) \
            { \
                assert(!std::isnan(m[i][j]) && "矩阵包含NaN值"); \
                assert(!std::isinf(m[i][j]) && "矩阵包含无穷大值"); \
            } \
        } \
    } while (0)

using namespace PSB;

namespace emoteplayer
{
#pragma region Physics

    class EasingForce
    {
    public:
        std::string name;
        Vector3 targetForce;
        Vector3 startForce;
        Vector3 currentForce;
        float duration;
        float elapsedTime;
        float easing;
        bool isActive;

        EasingForce() : duration(0), elapsedTime(0), easing(0), isActive(false) {}

        EasingForce(const std::string& n, const Vector3& target, float t, float e)
          : name(n),
            targetForce(target),
            startForce(Vector3::zero()),
            currentForce(Vector3::zero()),
            duration(t),
            elapsedTime(0),
            easing(e),
            isActive(true)
        {
        }

        void update(float deltaTime)
        {
            if (!isActive)
                return;

            elapsedTime += deltaTime;
            float progress = std::min(elapsedTime / duration, 1.0f);

            float t = applyEasing(progress, easing);
            currentForce = startForce + (targetForce - startForce) * t;

            if (progress >= 1.0f)
            {
                isActive = false;
            }
        }

        bool shouldRemove() const { return !isActive; }

    private:
        float applyEasing(float t, float easing)
        {
            if (easing < 0)
            {
                return 1.0f - std::cos((t * 3.14159265f) / 2.0f);
            }
            else if (easing > 0)
            {
                return std::sin((t * 3.14159265f) / 2.0f);
            }
            else
            {
                return t;
            }
        }
    };
    class OuterForceSystem
    {
    private:
        std::unordered_map<std::string, EasingForce> activeForces;

    public:
        void setOuterForce(
            const std::string& name, float ofx, float ofy, float time = 0, float easing = 0)
        {
            Vector3 targetForce(ofx, ofy, 0);

            if (time <= 0)
            {
                EasingForce force(name, targetForce, 0, 0);
                force.currentForce = targetForce;
                force.isActive = false;
                activeForces[name] = force;
            }
            else
            {
                EasingForce force(name, targetForce, time / 1000.0f, easing);
                activeForces[name] = force;
            }
        }

        void removeOuterForce(const std::string& name) { activeForces.erase(name); }

        void clearAllForces() { activeForces.clear(); }

        void update(float deltaTime)
        {
            for (auto it = activeForces.begin(); it != activeForces.end();)
            {
                it->second.update(deltaTime);
                if (it->second.shouldRemove())
                {
                    it = activeForces.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        Vector3 getTotalOuterForce()
        {
            Vector3 totalForce = Vector3::zero();
            for (auto& pair : activeForces)
            {
                totalForce = totalForce + pair.second.currentForce;
            }
            return totalForce;
        }

        bool hasForce(const std::string& name) const
        {
            return activeForces.find(name) != activeForces.end();
        }
    };
    class WindSystem
    {
    private:
        Vector3 windStart;
        Vector3 windGoal;
        Vector3 windDirection;
        float windSpeed;
        float windPowerMin;
        float windPowerMax;
        float currentWindPower;
        bool isWindActive;
        float timeAccumulator;

    public:
        WindSystem()
          : windSpeed(0),
            windPowerMin(0),
            windPowerMax(0),
            currentWindPower(0),
            isWindActive(false),
            timeAccumulator(0)
        {
        }

        void startWind(
            const Vector3& start, const Vector3& goal, float speed, float powMin, float powMax)
        {
            windStart = start;
            windGoal = goal;
            windSpeed = speed;
            windPowerMin = powMin;
            windPowerMax = powMax;
            windDirection = (goal - start).normalized();
            isWindActive = true;
            currentWindPower = powMin;
            timeAccumulator = 0;
        }

        void stopWind()
        {
            isWindActive = false;
            currentWindPower = 0.0f;
        }

        void update(float deltaTime)
        {
            if (!isWindActive)
                return;

            timeAccumulator += deltaTime;
            currentWindPower =
                windPowerMin + (windPowerMax - windPowerMin) *
                                   (0.5f + 0.5f * std::sin(timeAccumulator * windSpeed));
        }

        Vector3 getWindForceAtPosition(const Vector3& position)
        {
            if (!isWindActive)
                return Vector3::zero();

            float distanceFactor = calculateDistanceFactor(position);
            float powerVariation = 0.8f + 0.2f * noise(position.x, position.z);

            return windDirection * currentWindPower * distanceFactor * powerVariation;
        }

        bool isActive() const { return isWindActive; }

    private:
        float calculateDistanceFactor(const Vector3& position)
        {
            Vector3 closestPoint = findClosestPointOnLine(position, windStart, windGoal);
            float distance = (position - closestPoint).magnitude();
            return std::max(0.0f, 1.0f - distance / 50.0f);
        }

        Vector3 findClosestPointOnLine(const Vector3& point,
                                       const Vector3& lineStart,
                                       const Vector3& lineEnd)
        {
            Vector3 lineVec = lineEnd - lineStart;
            float lineLength = lineVec.magnitude();
            if (lineLength == 0)
                return lineStart;

            Vector3 lineDir = lineVec / lineLength;
            Vector3 pointVec = point - lineStart;
            float projection = pointVec.dot(lineDir);
            projection = std::max(0.0f, std::min(projection, lineLength));
            return lineStart + lineDir * projection;
        }

        float noise(float x, float z) { return (std::sin(x * 0.1f) + std::sin(z * 0.15f)) * 0.5f; }
    };
    class BustPhysicsSimulator
    {
    private:
        struct BustState
        {
            Vector3 restPosition; // 静止位置 (op)
            Vector3 position;     // 当前位置 (p)
            Vector3 velocity;     // 当前速度 (pv)
            float offset;         // 偏移量 (ofs)
        };

        BustState state;
        float spring;       // 弹簧系数
        float friction;     // 摩擦力
        float gravity;      // 重力
        float scale_x;      // X轴缩放
        float scale_y;      // Y轴缩放
        std::string var_lr; // 左右变量名
        std::string var_ud; // 上下变量名
        bool enabled;

    public:
        BustPhysicsSimulator()
          : spring(0),
            friction(0),
            gravity(0),
            scale_x(1),
            scale_y(1),
            enabled(false)
        {
        }

        void initialize(const std::string& baseLayer,
                        bool enable,
                        float fric,
                        float grav,
                        const Vector3& op,
                        const Vector3& p,
                        const Vector3& pv,
                        float ofs,
                        float spr,
                        float scl_x,
                        float scl_y,
                        const std::string& lr,
                        const std::string& ud)
        {
            state.restPosition = op;
            state.position = p;
            state.velocity = pv;
            state.offset = ofs;
            spring = spr;
            friction = fric;
            gravity = grav;
            scale_x = scl_x;
            scale_y = scl_y;
            var_lr = lr;
            var_ud = ud;
            enabled = enable;
        }

        void update(float deltaTime, const Vector3& externalForce)
        {
            if (!enabled)
                return;

            // 弹簧力 (胡克定律)
            Vector3 springForce = (state.restPosition - state.position) * spring;

            // 重力
            Vector3 gravityForce = Vector3(0, -gravity, 0);

            // 摩擦力
            Vector3 frictionForce = state.velocity * -friction;

            // 合力
            Vector3 totalForce = springForce + gravityForce + frictionForce + externalForce;

            // 更新物理状态
            state.velocity = state.velocity + totalForce * deltaTime;
            state.position = state.position + state.velocity * deltaTime;

            // 更新输出变量
            updateOutputVariables();
        }

        float getVariableValue(const std::string& varName) { return 0.0f; }

    private:
        void updateOutputVariables()
        {
            // 计算相对于静止位置的偏移
            Vector3 offsetFromRest = state.position - state.restPosition;

            // 应用缩放和基础偏移
            float bust_LR = offsetFromRest.x * scale_x + state.offset;
            float bust_UD = offsetFromRest.y * scale_y + state.offset;

            // 设置输出变量
            setVariableValue(var_lr, bust_LR);
            setVariableValue(var_ud, bust_UD);
        }

        void setVariableValue(const std::string& varName, float value) {}
    };
    class HairSegment
    {
    public:
        Vector3 basePosition;
        Vector3 position;
        Vector3 velocity;
        float scaleX;
        float scaleY;
        float length;

        HairSegment() : scaleX(1), scaleY(1), length(0) {}
    };
    class HairPhysicsSimulator
    {
    private:
        std::vector<HairSegment> segments;

        float b_rate;
        float gravity;
        float friction_x;
        float friction_y;
        float bend_spd;
        float bend_vol;
        float offset;
        bool v_bound;

        std::string var_lr;
        std::string var_lrm;
        std::string var_ud;

    public:
        HairPhysicsSimulator()
          : b_rate(0),
            gravity(0),
            friction_x(0),
            friction_y(0),
            bend_spd(0),
            bend_vol(0),
            offset(0),
            v_bound(false)
        {
        }

        void initialize(const std::vector<HairSegment>& segs,
                        float bRate,
                        float grav,
                        float fricX,
                        float fricY,
                        float bendSpd,
                        float bendVol,
                        float ofs,
                        bool vBound,
                        const std::string& lr,
                        const std::string& lrm,
                        const std::string& ud)
        {
            segments = segs;
            b_rate = bRate;
            gravity = grav;
            friction_x = fricX;
            friction_y = fricY;
            bend_spd = bendSpd;
            bend_vol = bendVol;
            offset = ofs;
            v_bound = vBound;
            var_lr = lr;
            var_lrm = lrm;
            var_ud = ud;
        }

        void update(float deltaTime, const Vector3& externalForce)
        {
            for (size_t i = 0; i < segments.size(); ++i)
            {
                updateSegment(segments[i], deltaTime, externalForce, i);
            }
            updateOutputVariables();
        }

        float getVariableValue(const std::string& varName) { return 0.0f; }

    private:
        void updateSegment(HairSegment& segment,
                           float deltaTime,
                           const Vector3& externalForce,
                           int segmentIndex)
        {
            Vector3 springForce = (segment.basePosition - segment.position) * b_rate;
            Vector3 gravityForce = Vector3(0, -gravity, 0);

            float windInfluence = 0.5f + 0.5f * (segmentIndex / (float)(segments.size() - 1));
            Vector3 externalForceWithWind = externalForce * (1.0f + windInfluence);

            Vector3 frictionForce;
            frictionForce.x = -segment.velocity.x * friction_x;
            frictionForce.y = -segment.velocity.y * friction_y;
            frictionForce.z = -segment.velocity.z * std::min(friction_x, friction_y);

            Vector3 bendForce = calculateBendForce(segment, segmentIndex);

            Vector3 totalForce =
                springForce + gravityForce + externalForceWithWind + frictionForce + bendForce;

            segment.velocity = segment.velocity + totalForce * deltaTime;

            if (v_bound)
            {
                segment.velocity = clampVelocity(segment.velocity);
            }

            segment.position = segment.position + segment.velocity * deltaTime;
        }

        Vector3 calculateBendForce(const HairSegment& segment, int segmentIndex)
        {
            Vector3 force = Vector3::zero();

            if (segmentIndex > 0)
            {
                const HairSegment& prevSegment = segments[segmentIndex - 1];
                Vector3 dir = (segment.position - prevSegment.position).normalized();
                force = dir * bend_vol * bend_spd;
            }

            return force;
        }

        Vector3 clampVelocity(const Vector3& velocity)
        {
            float maxSpeed = 10.0f;
            float currentSpeed = velocity.magnitude();

            if (currentSpeed > maxSpeed && currentSpeed > 0)
            {
                return velocity * (maxSpeed / currentSpeed);
            }
            return velocity;
        }

        void updateOutputVariables()
        {
            if (segments.size() < 2)
                return;

            Vector3 segment1Offset = segments[0].position - segments[0].basePosition;
            float lr1 = segment1Offset.x * segments[0].scaleX;
            float ud1 = segment1Offset.y * segments[0].scaleY;

            Vector3 segment2Offset = segments[1].position - segments[1].basePosition;
            float lr2 = segment2Offset.x * segments[1].scaleX;
            float ud2 = segment2Offset.y * segments[1].scaleY;

            float hair_LR = (lr1 + lr2) * 0.5f + offset;
            float hair_LR_M = lr1 * 0.7f + lr2 * 0.3f + offset;
            float hair_UD = (ud1 + ud2) * 0.5f + offset;

            setVariableValue(var_lr, hair_LR);
            setVariableValue(var_lrm, hair_LR_M);
            setVariableValue(var_ud, hair_UD);
        }

        void setVariableValue(const std::string& varName, float value) {}
    };
    class CompletePhysicsSystem
    {
    private:
        HairPhysicsSimulator hairPhysics;
        BustPhysicsSimulator bustPhysics;
        WindSystem windSystem;
        OuterForceSystem outerForceSystem;

    public:
        void update(float deltaTime,
                    const Vector3& headMovement,
                    const Vector3& headPosition,
                    const Vector3& bodyMovement)
        {
            // 更新外力系统
            outerForceSystem.update(deltaTime);

            // 获取风力和外力
            Vector3 windForce = windSystem.getWindForceAtPosition(headPosition);
            Vector3 outerForce = outerForceSystem.getTotalOuterForce();

            // 合并所有外力
            Vector3 totalExternalForce = calculateTotalForce(headMovement, windForce, outerForce);

            // 更新头发物理（使用头部运动）
            hairPhysics.update(deltaTime, totalExternalForce);

            // 更新胸部物理（使用身体运动）
            Vector3 bustExternalForce = calculateBustForce(bodyMovement, windForce, outerForce);
            bustPhysics.update(deltaTime, bustExternalForce);

            // 更新风系统
            windSystem.update(deltaTime);
        }

        // 风系统控制
        void startWind(
            const Vector3& start, const Vector3& goal, float speed, float powMin, float powMax)
        {
            windSystem.startWind(start, goal, speed, powMin, powMax);
        }

        void stopWind() { windSystem.stopWind(); }

        // 外力系统控制
        void setOuterForce(
            const std::string& name, float ofx, float ofy, float time = 0, float easing = 0)
        {
            outerForceSystem.setOuterForce(name, ofx, ofy, time, easing);
        }

        void removeOuterForce(const std::string& name) { outerForceSystem.removeOuterForce(name); }

        void clearAllOuterForces() { outerForceSystem.clearAllForces(); }

        // 头发系统初始化
        void initializeHairPhysics(const std::vector<HairSegment>& segs,
                                   float bRate,
                                   float grav,
                                   float fricX,
                                   float fricY,
                                   float bendSpd,
                                   float bendVol,
                                   float ofs,
                                   bool vBound,
                                   const std::string& lr,
                                   const std::string& lrm,
                                   const std::string& ud)
        {
            hairPhysics.initialize(segs, bRate, grav, fricX, fricY, bendSpd, bendVol, ofs, vBound,
                                   lr, lrm, ud);
        }

        // 胸部系统初始化
        void initializeBustPhysics(const std::string& baseLayer,
                                   bool enable,
                                   float fric,
                                   float grav,
                                   const Vector3& op,
                                   const Vector3& p,
                                   const Vector3& pv,
                                   float ofs,
                                   float spr,
                                   float scl_x,
                                   float scl_y,
                                   const std::string& lr,
                                   const std::string& ud)
        {
            bustPhysics.initialize(baseLayer, enable, fric, grav, op, p, pv, ofs, spr, scl_x, scl_y,
                                   lr, ud);
        }

        bool isWindActive() const { return windSystem.isActive(); }

    private:
        Vector3 calculateTotalForce(const Vector3& headMovement,
                                    const Vector3& windForce,
                                    const Vector3& outerForce)
        {
            Vector3 totalForce;

            // 头部运动的影响（对头发）
            totalForce.x = headMovement.x * 2.0f;
            totalForce.y = headMovement.y * 1.0f;
            totalForce.z = headMovement.z * 1.5f;

            // 叠加风力和外力
            totalForce = totalForce + windForce + outerForce;

            return totalForce;
        }

        Vector3 calculateBustForce(const Vector3& bodyMovement,
                                   const Vector3& windForce,
                                   const Vector3& outerForce)
        {
            Vector3 totalForce;

            // 身体运动的影响（对胸部）
            totalForce.x = bodyMovement.x * 1.5f;
            totalForce.y = bodyMovement.y * 1.0f;
            totalForce.z = bodyMovement.z * 1.0f;

            // 叠加风力和外力（胸部对风的反应较小）
            totalForce = totalForce + (windForce * 0.3f) + outerForce;

            return totalForce;
        }
    };
    class ExampleUsage
    {
    private:
        CompletePhysicsSystem physicsSystem;

    public:
        void initialize()
        {
            // 初始化头发系统
            std::vector<HairSegment> hairSegments(2);

            hairSegments[0].basePosition = Vector3(0, 64, 0);
            hairSegments[0].position = Vector3(0, 225.684357, 0);
            hairSegments[0].velocity = Vector3(0, -4.91552055E-06, 0);
            hairSegments[0].scaleX = 0.75f;
            hairSegments[0].scaleY = 2.0f;
            hairSegments[0].length = 64.0f;

            hairSegments[1].basePosition = Vector3(0, 112, 0);
            hairSegments[1].position = Vector3(0, 274.228119, 0);
            hairSegments[1].velocity = Vector3(0, 0.543750048, 0);
            hairSegments[1].scaleX = 0.25f;
            hairSegments[1].scaleY = 3.0f;
            hairSegments[1].length = 48.0f;

            physicsSystem.initializeHairPhysics(
                hairSegments, 0.00371093745f, 0.6f, 0.046875f, 0.09375f, 0.2f, 3.0f, -161.684357f,
                true, "hair_LR_front", "hair_LR_M_front", "hair_UD_front");

            // 初始化胸部系统
            physicsSystem.initializeBustPhysics("center_bust",               // baseLayer
                                                true,                        // enabled
                                                0.06f,                       // friction
                                                0.1f,                        // gravity
                                                Vector3(0, 0, 0),            // op
                                                Vector3(0, 3.200208, 0),     // p
                                                Vector3(0, 9.272695E-06, 0), // pv
                                                3.1996305f,                  // ofs
                                                0.03125f,                    // spring
                                                1.0f,                        // scale_x
                                                2.0f,                        // scale_y
                                                "bust_LR",                   // var_lr
                                                "bust_UD"                    // var_ud
            );
        }

        void update(float deltaTime,
                    const Vector3& headPosition,
                    const Vector3& headMovement,
                    const Vector3& bodyMovement)
        {
            physicsSystem.update(deltaTime, headMovement, headPosition, bodyMovement);
        }

        void triggerWind()
        {
            physicsSystem.startWind(Vector3(-100, 50, 0), Vector3(100, 50, 0), 2.0f, 0.5f, 3.0f);
        }

        void applyQuickShake() { physicsSystem.setOuterForce("shake", 5.0f, 2.0f, 200, 1.0f); }

        void applyGentleSway() { physicsSystem.setOuterForce("sway", -3.0f, 1.0f, 1000, -1.0f); }

        void stopAllForces()
        {
            physicsSystem.stopWind();
            physicsSystem.clearAllOuterForces();
        }
    };

    bustControl::bustControl(emotefile* filePtr, uint32_t startOffset)
    {
        // dic
        std::map<std::string, uint32_t> _rootData;
        filePtr->parseObject(_rootData, startOffset);
        // friction/gravity/spring
        filePtr->parseReal(friction, _rootData["friction"]);
        filePtr->parseReal(gravity, _rootData["gravity"]);
        filePtr->parseReal(spring, _rootData["spring"]);
        //scale_x/y
        filePtr->parseReal(scale_x, _rootData["scale_x"]);
        filePtr->parseReal(scale_y, _rootData["scale_y"]);
        //lable
        filePtr->parseString(var_lr, _rootData["var_lr"]);
        filePtr->parseString(var_ud, _rootData["var_ud"]);
        //param
        if (filePtr->parseObject(_rootData, _rootData["param"]))
        {
            filePtr->parseReal(param.ofs, _rootData["ofs"]);
            // op
            std::map<std::string, uint32_t> _tmpData;
            if (filePtr->parseObject(_tmpData, _rootData["op"]))
            {
                filePtr->parseReal(param.op.x, _tmpData["x"]);
                filePtr->parseReal(param.op.y, _tmpData["y"]);
                filePtr->parseReal(param.op.z, _tmpData["z"]);
            }
            // p
            if (filePtr->parseObject(_tmpData, _rootData["p"]))
            {
                filePtr->parseReal(param.p.x, _tmpData["x"]);
                filePtr->parseReal(param.p.y, _tmpData["y"]);
                filePtr->parseReal(param.p.z, _tmpData["z"]);
            }
            // pv
            if (filePtr->parseObject(_tmpData, _rootData["pv"]))
            {
                filePtr->parseReal(param.pv.x, _tmpData["x"]);
                filePtr->parseReal(param.pv.y, _tmpData["y"]);
                filePtr->parseReal(param.pv.z, _tmpData["z"]);
            }
        }
    }
    bustControl::~bustControl()
    {

    }

    uniControl::uniControl(emotefile* filePtr, uint32_t startOffset)
    {
        // dic
        std::map<std::string, uint32_t> _rootData;
        filePtr->parseObject(_rootData, startOffset);
        // b_rate/bend_spd/bend_vol/friction_x/friction_y/gravity
        filePtr->parseReal(b_rate, _rootData["b_rate"]);
        filePtr->parseReal(bend_spd, _rootData["bend_spd"]);
        filePtr->parseReal(bend_vol, _rootData["bend_vol"]);
        filePtr->parseReal(friction_x, _rootData["friction_x"]);
        filePtr->parseReal(friction_y, _rootData["friction_y"]);
        filePtr->parseReal(gravity, _rootData["gravity"]);
        // ud_eft/v_bound
        filePtr->parseReal(ud_eft, _rootData["ud_eft"]);
        filePtr->parseReal(v_bound, _rootData["v_bound"]);
        // lable
        filePtr->parseString(var_lr, _rootData["var_lr"]);
        filePtr->parseString(var_lrm, _rootData["var_lrm"]);
        filePtr->parseString(var_ud, _rootData["var_ud"]);
        // param
        std::vector<uint32_t> _sx, _sy, _len;
        filePtr->parseList(_sx, _rootData["scale_x"]);
        filePtr->parseList(_sy, _rootData["scale_y"]);
        filePtr->parseList(_len, _rootData["length"]);
        if (filePtr->parseObject(_rootData, _rootData["param"]))
        {
            // ofs/bendR/bendS/op
            filePtr->parseReal(ofs, _rootData["ofs"]);
            filePtr->parseReal(bendR, _rootData["bendR"]);
            filePtr->parseReal(bendS, _rootData["bendS"]);
            std::map<std::string, uint32_t> _tmpData;
            if (filePtr->parseObject(_tmpData, _rootData["op"]))
            {
                filePtr->parseReal(op.x, _tmpData["x"]);
                filePtr->parseReal(op.y, _tmpData["y"]);
                filePtr->parseReal(op.z, _tmpData["z"]);
            }
            // for get
            std::vector<uint32_t> _bp, _p, _pv;
            filePtr->parseList(_bp, _rootData["bp"]);
            filePtr->parseList(_p, _rootData["p"]);
            filePtr->parseList(_pv, _rootData["pv"]);
            // bp/p/pv
            int cts = std::min(_bp.size(), std::min(_p.size(), _pv.size()));
            for (int i = 0; i < cts; i++)
            {
                uniSegment _psD;
                filePtr->parseObject(_tmpData, _bp.at(i));
                filePtr->parseReal(_psD.bp.x, _tmpData["x"]);
                filePtr->parseReal(_psD.bp.y, _tmpData["y"]);
                filePtr->parseReal(_psD.bp.z, _tmpData["z"]);
                filePtr->parseObject(_tmpData, _p.at(i));
                filePtr->parseReal(_psD.p.x, _tmpData["x"]);
                filePtr->parseReal(_psD.p.y, _tmpData["y"]);
                filePtr->parseReal(_psD.p.z, _tmpData["z"]);
                filePtr->parseObject(_tmpData, _pv.at(i));
                filePtr->parseReal(_psD.pv.x, _tmpData["x"]);
                filePtr->parseReal(_psD.pv.y, _tmpData["y"]);
                filePtr->parseReal(_psD.pv.z, _tmpData["z"]);
                filePtr->parseReal(_psD.scale_x, _sx.at(i));
                filePtr->parseReal(_psD.scale_y, _sy.at(i));
                filePtr->parseReal(_psD.length, _len.at(i));
                segments.push_back(_psD);
            }
        }
    }
    uniControl::~uniControl()
    {

    }

    // 物理玩不明白，这都是ai写的 分别用于parts/hair/bust
    SmoothPeriodicRandom::SmoothPeriodicRandom()
      : noise_seed(std::rand() / float(RAND_MAX) * 1000.0f)
    {
    }
    float SmoothPeriodicRandom::generate(float tick, float a, float b, float period)
    {
        // 基础周期
        float base = std::sin(tick * 2.0f * 3.14159f / period);

        // 平滑随机偏移（使用噪声函数）
        float random_offset = std::sin(tick * 0.3f + noise_seed) * 0.4f;

        // 组合
        float value = base * 0.6f + random_offset * 0.4f;

        // 优雅的映射
        return a + (b - a) * (0.5f + 0.4f * std::tanh(value));
    }
    HairSwaySimulator::HairSwaySimulator()
    {
        // 设置多个频率分量模拟不同发丝的飘动
        frequencies = {0.8f, 1.2f, 1.7f, 2.3f, 0.5f};
        amplitudes = {0.5f, 0.3f, 0.15f, 0.08f, 0.2f};

        phases.resize(frequencies.size());

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dist(0.0f, 6.28318f);

        for (auto& phase : phases)
        {
            phase = dist(gen);
        }

        wind_seed = dist(gen);
        wind.active = false;
        wind.progress = 0.0f;
    }
    // 风开始
    void HairSwaySimulator::startWind(
        glm::vec2 start, glm::vec2 goal, float speed, float powMin, float powMax)
    {
        wind.start = start;
        wind.goal = goal;
        wind.speed = speed;
        wind.powMin = powMin;
        wind.powMax = powMax;
        wind.progress = 0.0f;
        wind.active = true;

        // 重置风种子以获得新的随机模式
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dist(0.0f, 6.28318f);
        wind_seed = dist(gen);
    }
    // 停止风
    void HairSwaySimulator::stopWind()
    {
        wind.active = false;
        wind.progress = 0.0f;
    }
    // 获取当前风力强度（基于位置和进度）
    float HairSwaySimulator::getCurrentWindStrength(glm::vec2 hairPosition)
    {
        if (!wind.active)
            return 0.0f;

        // 计算风路径的方向和距离
        glm::vec2 windDir = glm::normalize(wind.goal - wind.start);
        glm::vec2 toHair = hairPosition - wind.start;

        // 计算头发在风路径上的投影距离
        float projection = glm::dot(toHair, windDir);
        float windLength = glm::length(wind.goal - wind.start);

        // 计算风的影响范围（风宽度）
        float windWidth = windLength * 0.3f;
        float lateralDist = glm::length(toHair - windDir * projection);

        // 如果头发在风路径之外，影响减弱
        float lateralFactor = std::max(0.0f, 1.0f - lateralDist / windWidth);

        // 计算风进程的影响
        float windFront = wind.progress * windLength;
        float distanceToFront = std::abs(projection - windFront);

        // 风前沿的影响（高斯分布）
        float frontFactor =
            std::exp(-distanceToFront * distanceToFront / (windWidth * windWidth * 0.5f));

        // 组合所有因素
        float strength = wind.powMin + (wind.powMax - wind.powMin) * wind.progress;
        strength *= lateralFactor * frontFactor;

        return strength;
    }
    // 获取风的方向
    glm::vec2 HairSwaySimulator::getWindDirection()
    {
        if (!wind.active)
            return glm::vec2(1.0f, 0.0f); // 默认方向
        return glm::normalize(wind.goal - wind.start);
    }
    float HairSwaySimulator::generate(float tick, glm::vec2 hairPosition, float a, float b)
    {
        // 更新风进程
        if (wind.active)
        {
            wind.progress += wind.speed * 0.01f; // 调整速度系数
            if (wind.progress >= 1.0f)
            {
                wind.active = false; // 风到达目标
            }
        }

        float result = 0.0f;

        // 1. 基础飘动（多个频率合成）
        for (size_t i = 0; i < frequencies.size(); ++i)
        {
            float freq = frequencies[i] * (0.9f + 0.2f * std::sin(tick * 0.01f));
            result += amplitudes[i] * std::sin(tick * freq * 0.05f + phases[i]);
        }

        // 2. 风的影响
        float wind_strength = getCurrentWindStrength(hairPosition);
        glm::vec2 wind_dir = getWindDirection();

        if (wind_strength > 0.0f)
        {
            // 主风浪
            float wind_wave = std::sin(tick * 0.3f + wind_seed) * wind_strength;

            // 风湍流（高频分量）
            float wind_turbulence = std::sin(tick * 1.5f + wind_seed * 2.0f) * wind_strength * 0.3f;

            // 风向影响（主要影响水平分量）
            float wind_direction_factor = std::abs(wind_dir.x); // 假设x是水平方向

            result += (wind_wave + wind_turbulence) * wind_direction_factor;
        }

        // 3. 随机扰动
        // float random_jitter = std::sin(tick * 5.0f + wind_seed * 2.0f) *
        //    std::cos(tick * 3.0f + wind_seed) * 0.1f;
        // result += random_jitter;

        // 4. 缓慢的基线漂移
        float base_drift = std::sin(tick * 0.02f) * 0.2f;
        result += base_drift;

        // 映射到 [a, b] 范围
        float normalized = std::tanh(result * 1.5f);
        return a + (b - a) * (0.5f + 0.4f * normalized);
    }
    // 检查风是否活跃
    bool HairSwaySimulator::isWindActive()
    {
        return wind.active;
    }
    // 获取风进度
    float HairSwaySimulator::getWindProgress()
    {
        return wind.progress;
    }
    BreastJiggleSimulator::SpringDamper::SpringDamper(float stiff,
                                                      float damp,
                                                      float m )
      : position(0),
        velocity(0),
        target(0),
        stiffness(stiff),
        damping(damp),
        mass(m)
    {
    }
    void BreastJiggleSimulator::SpringDamper::update(float delta_time)
    {
        // 弹簧阻尼器物理更新
        float acceleration = (target - position) * stiffness - velocity * damping;
        acceleration /= mass;
        velocity += acceleration * delta_time;
        position += velocity * delta_time;
    }
    BreastJiggleSimulator::SpringDamper vertical_spring;   // 垂直运动
    BreastJiggleSimulator::SpringDamper horizontal_spring; // 水平运动
    BreastJiggleSimulator::SpringDamper bounce_spring;     // 弹跳运动
    float base_movement;                                   // 基础运动（如呼吸）
    float last_body_movement;                              // 上一帧身体运动
    BreastJiggleSimulator::BreastJiggleSimulator()
      : vertical_spring(0.15f, 0.08f, 1.1f),   // 垂直：较软，较重
        horizontal_spring(0.25f, 0.12f, 0.9f), // 水平：较硬，较轻
        bounce_spring(0.3f, 0.2f, 0.8f),       // 弹跳：最硬，恢复快
        base_movement(0),
        last_body_movement(0)
    {
    }
    float BreastJiggleSimulator::generate(
        float tick, float body_movement, float a, float b, float delta_time)
    {
        // 1. 基础生理运动（呼吸等）
        base_movement = std::sin(tick * 0.3f) * 0.1f + std::sin(tick * 0.7f) * 0.05f;

        // 2. 身体运动引起的驱动（走路、跑步等）
        float movement_delta = body_movement - last_body_movement;
        float movement_force = movement_delta * 2.0f; // 运动变化产生力

        // 3. 更新各个弹簧系统
        // 垂直运动（主要响应上下运动）
        vertical_spring.target = base_movement + movement_force * 0.8f;
        vertical_spring.update(delta_time);

        // 水平运动（响应左右运动）
        horizontal_spring.target = std::sin(tick * 0.5f) * 0.05f + movement_force * 0.3f;
        horizontal_spring.update(delta_time);

        // 弹跳运动（跳跃等冲击）
        if (std::abs(movement_delta) > 0.5f)
        {
            bounce_spring.target = movement_force * 1.5f;
        }
        else
        {
            bounce_spring.target = 0;
        }
        bounce_spring.update(delta_time);

        // 4. 合成最终运动（带相位差模拟滞后）
        float vertical = vertical_spring.position;
        float horizontal = horizontal_spring.position * 0.7f;
        float bounce = bounce_spring.position * 0.4f;

        // 添加轻微随机扰动（模拟肌肉微颤）
        float micro_tremor = std::sin(tick * 8.0f) * 0.02f * std::cos(tick * 3.0f);

        float combined = vertical + horizontal + bounce + micro_tremor;

        last_body_movement = body_movement;

        // 5. 非线性映射（模拟软组织特性）
        float softened = std::tanh(combined * 2.0f) * 0.5f; // 软化曲线
        return a + (b - a) * (0.5f + softened);
    }
    // 外部冲击（如跳跃落地）
    void BreastJiggleSimulator::addImpulse(float force)
    {
        bounce_spring.velocity += force * 0.5f;
        vertical_spring.velocity += force * 0.3f;
    }
} // namespace emoteplayer
