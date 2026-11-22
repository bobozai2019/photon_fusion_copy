using UnityEngine;

namespace Netcode
{
    public enum ForecastCorrection { Velocity, Force, Blend }
    public sealed class ForecastBody
    {
        int generation = -1;
        public void ResetBaseline(int next) { generation=~next; }
        public ForecastCorrection Mode;
        public float MaxForecast = .1f, Stiffness = 50, Damping = 10, BlendFactor = .1f;
        public bool IncludeGravity = true;
        public object[] Sample(Rigidbody2D body, int teleport, int phase)
        {
            return new object[] {body.position, body.rotation * Mathf.Deg2Rad, body.linearVelocity,
                body.angularVelocity * Mathf.Deg2Rad, body.IsSleeping(), teleport, phase};
        }
        public void Apply(Rigidbody2D body, object[] fields, double elapsed)
        {
            bool sleep = (bool)fields[4];
            bool snap = generation != (int)fields[5] || sleep;
            generation = (int)fields[5];
            var age = snap ? 0 : Mathf.Clamp((float)elapsed, 0, Mathf.Max(0,MaxForecast));
            var v = (Vector2)fields[2];
            var gravity = IncludeGravity ? Physics2D.gravity * body.gravityScale : Vector2.zero;
            var target = (Vector2)fields[0] + v*age + gravity*(.5f*age*age);
            v += gravity*age;
            var spin = (float)fields[3]*Mathf.Rad2Deg;
            var angle = (float)fields[1]*Mathf.Rad2Deg + age*spin;
            body.WakeUp();
            if (snap) { body.position = target; body.rotation = angle; body.linearVelocity = v; }
            else if (Mode == ForecastCorrection.Blend) {
                body.position = Vector2.Lerp(body.position,target,BlendFactor);
                body.rotation = Mathf.LerpAngle(body.rotation,angle,BlendFactor);
                body.linearVelocity = v;
            } else if (Mode == ForecastCorrection.Force) {
                body.AddForce(body.mass*(Stiffness*(target-body.position)+Damping*(v-body.linearVelocity)));
            } else body.linearVelocity = v + (target-body.position)*6;
            if (!snap && Mode == ForecastCorrection.Force)
                body.AddTorque(body.inertia*Mathf.Deg2Rad*(Stiffness*Mathf.DeltaAngle(body.rotation,angle)+Damping*(spin-body.angularVelocity)));
            else body.angularVelocity = spin + (!snap && Mode == ForecastCorrection.Velocity ? Mathf.DeltaAngle(body.rotation,angle)*6 : 0);
            if (sleep) body.Sleep();
        }
    }
}
