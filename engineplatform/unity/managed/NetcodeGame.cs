using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

namespace Netcode
{
    public enum RpcStatus { Received, OutOfScope, PeerLeft, Expired, ObjectGone, RoundEnded, LocalSend, QueueFull }
    public enum MigrationPhase { Running, Voting, Fenced, Ended, Retiring }
    public enum InterestKind { Global, Area, User, Inherit }
    public enum RpcDirection { Server, Client, NetMulticast, Remote }
    public enum Ownership { Transaction, Dynamic, PlayerAttached, MasterClient, GameGlobal }
    public enum EventKind { Ready, Hidden, Destroyed, Changed, Grant, Request, Result, PredictionFailed, MapAdded, MapRemoved, SpawnResult, Reconciled, SessionReplaced }
    [StructLayout(LayoutKind.Sequential)]
    public struct GameEvent { public EventKind Kind; public uint Object, Player, Epoch, Sequence, Status; }
    [StructLayout(LayoutKind.Sequential)]
    public struct GameFrame { public ulong Round; public uint Player, Seats; public double Time, Rtt, Rate, Offset; }

    public sealed class NetcodeGame : IDisposable
    {
        const string Plugin = "netcode_unity";
        const CallingConvention ABI = CallingConvention.Cdecl;
        [DllImport(Plugin, CallingConvention=ABI)] static extern IntPtr ng_open([MarshalAs(UnmanagedType.LPUTF8Str)] string config, int host, [MarshalAs(UnmanagedType.LPUTF8Str)] string address, int port, int capacity, int spectator);
        [DllImport(Plugin, CallingConvention=ABI)] static extern IntPtr ng_error();
        [DllImport(Plugin, CallingConvention=ABI)] static extern void ng_close(IntPtr game);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_model(IntPtr game, ushort schema, ushort[] fields, int count, int policy, uint cooldown);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_start(IntPtr game);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_pump(IntPtr game, out GameFrame frame);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_event(IntPtr game, out GameEvent entry);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_fields(IntPtr game, ushort schema, int index, out int kind, out ushort id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern ushort ng_schema(IntPtr game, uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_spawn(IntPtr game, ushort schema, byte[] data, int n, uint scene, ulong key, uint parent, out uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_submit(IntPtr game, uint id, byte[] data, int n);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_read(IntPtr game, uint id, int rendered, [Out] byte[] data, int n);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_claim(IntPtr game, uint id, int drop);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_answer(IntPtr game, ref GameEvent entry, int approve);
        [DllImport(Plugin, CallingConvention=ABI)] static extern uint ng_writer(IntPtr game, uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_loaded(IntPtr game, uint scene, uint generation);

        [DllImport(Plugin, CallingConvention=ABI)] static extern uint ng_revision(IntPtr game, uint id, out double time);
        public uint Revision(uint id, out double sampleTime) => ng_revision(Handle, id, out sampleTime);

        [DllImport(Plugin, CallingConvention=ABI)] static extern IntPtr ng_core_build_id();
        [DllImport(Plugin, CallingConvention=ABI)] static extern IntPtr ng_platform_build_id();
        public static string SdkBuildId => Marshal.PtrToStringAnsi(ng_core_build_id()) + "/" + Marshal.PtrToStringAnsi(ng_platform_build_id());

        IntPtr handle;
        public bool IsHost { get { Status(out _,out int host,out _,out _); return host!=0; } }
        public uint Term { get { Status(out uint term,out _,out _,out _); return term; } }
        public uint SavedTick { get { Status(out _,out _,out uint saved,out _); return saved; } }
        public bool Armed { get { Status(out _,out _,out _,out int armed); return armed!=0; } }
        public MigrationPhase Phase => Status(out _,out _,out _,out _);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_migration(IntPtr game,out uint term,out int host,out uint saved,out int armed);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_standby(IntPtr game,uint player,[MarshalAs(UnmanagedType.LPUTF8Str)] string address,int port);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_handoff(IntPtr game);
        MigrationPhase Status(out uint term,out int host,out uint saved,out int armed)
            => (MigrationPhase)ng_migration(Handle,out term,out host,out saved,out armed);
        public bool AddStandby(uint player,string address,int port) => ng_standby(Handle,player,address,port)==1;
        public bool Handoff() => ng_handoff(Handle)==1;
        readonly Dictionary<ushort, HashSet<ushort>> writable = new Dictionary<ushort, HashSet<ushort>>();
        readonly byte[] buffer = new byte[1176];
        public event Action<GameEvent> Changed;
        public event Action<uint, uint, ushort, byte[]> RpcReceived;
        [DllImport(Plugin,CallingConvention=ABI)] static extern int ng_bind_method(IntPtr game,[MarshalAs(UnmanagedType.LPUTF8Str)] string name,ushort method,ushort args,int dir,int reliable,int writer,int room);
        [DllImport(Plugin,CallingConvention=ABI)] static extern int ng_call_method(IntPtr game,[MarshalAs(UnmanagedType.LPUTF8Str)] string name,uint id,byte[] data,int size,uint target,ulong token);
        [DllImport(Plugin,CallingConvention=ABI)] static extern int ng_next_typed(IntPtr game,out uint id,out uint sender,out ushort method,out ushort args,[Out] byte[] bytes,int capacity,out int size);
        [DllImport(Plugin,CallingConvention=ABI)] static extern int ng_submit_fields(IntPtr game,uint id,byte[] data,int size);
        readonly Dictionary<string,ushort> signatures = new Dictionary<string,ushort>(StringComparer.Ordinal);
        readonly Dictionary<ushort,Action<uint,uint,object[]>> handlers = new Dictionary<ushort,Action<uint,uint,object[]>>();
        public bool BindMethod(string name,ushort id,ushort argSchema,RpcDirection direction,bool reliable,
                               Action<uint,uint,object[]> handler,bool writer=false,bool room=false) {
            if (string.IsNullOrEmpty(name) || name.IndexOf('\0')>=0 ||
                ng_bind_method(Handle,name,id,argSchema,(int)direction,reliable?1:0,writer?1:0,room?1:0)!=1) return false;
            signatures.Add(name,argSchema); handlers.Add(id,handler);
            return true;
        }
        static readonly Type[] fieldTypes = { typeof(float),typeof(bool),typeof(int),typeof(Vector2),typeof(Vector3),
            typeof(Quaternion),typeof(string),typeof(int[]),typeof(float[]),typeof(Vector4),typeof(Color),typeof(ulong),
            typeof(byte[]),typeof(Vector2[]),typeof(Dictionary<int,int>),typeof(LinkedList<int>) };
        public bool BindCallable(string name,ushort id,ushort schema,RpcDirection direction,bool reliable,
                                 Delegate handler,bool writer=false,bool room=false) {
            if(handler==null) return false;
            var invoke=handler.GetType().GetMethod("Invoke");
            var parameters=invoke.GetParameters();
            if(invoke.ReturnType!=typeof(void) || parameters.Length<2 || parameters[0].ParameterType!=typeof(uint) ||
               parameters[1].ParameterType!=typeof(uint)) return false;
            int index=0;
            while(ng_fields(Handle,schema,index,out int kind,out _)==1) {
                if(index+2>=parameters.Length || parameters[index+2].ParameterType!=fieldTypes[kind]) return false;
                index++;
            }
            if(parameters.Length!=index+2) return false;
            return BindMethod(name,id,schema,direction,reliable,(sender,obj,args)=>{
                var call=new object[args.Length+2]; call[0]=sender; call[1]=obj;
                Array.Copy(args,0,call,2,args.Length);
                handler.DynamicInvoke(call);
            },writer,room);
        }
        public bool CallMethod(string name,uint id,params object[] args) => CallTracked(name,id,0,0,args);
        public bool CallTracked(string name,uint id,uint target,ulong token,params object[] args) {
            if (name==null || args==null || !signatures.TryGetValue(name,out ushort schema)) return false;
            byte[] data;
            try { data=Encode(schema,false,args); }
            catch (InvalidCastException) { return false; }
            catch (ArgumentException) { return false; }
            catch (OverflowException) { return false; }
            return data.Length<=252 && ng_call_method(Handle,name,id,data,data.Length,target,token)==1;
        }
        [DllImport(Plugin,CallingConvention=ABI)] static extern int ng_sync_replica(IntPtr game,uint id,byte[] data,int size,[Out] byte[] dst,int capacity,out int count,out double age,int forecast);
        [DllImport(Plugin,CallingConvention=ABI)] static extern void ng_unbind_replica(IntPtr game,uint id);
        public void UnbindReplica(uint id) { if(handle!=IntPtr.Zero) ng_unbind_replica(Handle,id); }
        public int SyncReplica(uint id,object[] sample,out object[] fields,out double age,bool forecast=false) {
            fields=null; age=0;
            if(handle==IntPtr.Zero) return 0;
            ushort schema=ng_schema(Handle,id);
            if(schema==0) { UnbindReplica(id); return 0; }
            var data=Encode(schema,false,sample);
            int mode=ng_sync_replica(Handle,id,data,data.Length,buffer,buffer.Length,out int count,out age,forecast?1:0);
            if(mode>0) fields=Decode(schema,buffer,count);
            return mode;
        }
        public bool SubmitFields(uint id,params object[] fields) {
            var schema=ng_schema(Handle,id);
            if (schema==0) return false;
            var data=Encode(schema,false,fields);
            return ng_submit_fields(Handle,id,data,data.Length)==1;
        }
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_spawn_mode(IntPtr game, ushort schema, byte[] data, int n, uint scene, ulong key, uint parent, int mode, out uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_register_rpc(IntPtr game, int method, int direction, int reliable, int limit, int writer, int room);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_invoke_rpc(IntPtr game, uint id, ushort method, byte[] data, int size, uint target);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_next_rpc(IntPtr game, out uint id, out uint sender, out ushort method, [Out] byte[] data, int capacity, out int size);
        public bool RegisterRpc(ushort method, RpcDirection direction, bool reliable, int maxBytes, bool writer = false, bool room = false)
            => ng_register_rpc(Handle, method, (int)direction, reliable ? 1 : 0, maxBytes, writer ? 1 : 0, room ? 1 : 0) == 1;
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_invoke_tracked(IntPtr game,uint id,ushort method,byte[] data,int size,uint target,ulong token);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_rpc_outcome(IntPtr game,out ulong token,out uint player,out uint id,out ushort method,out int status);
        public event Action<ulong,uint,uint,ushort,RpcStatus> RpcOutcome;
        public bool InvokeRpc(uint id, ushort method, byte[] data, uint target = 0, ulong token = 0)
            => data != null && ng_invoke_tracked(Handle, id, method, data, data.Length, target,token) == 1;
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_distribute(IntPtr game, uint id, int kind, ulong[] keys, int count, int divisor);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_subscribe(IntPtr game, uint player, int kind, ulong[] keys, int[] rates, int count);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_player_ttl(IntPtr game, long ms);
        public bool Distribute(uint id, InterestKind kind, ulong[] keys, int divisor=1)
            => keys != null && ng_distribute(Handle,id,(int)kind,keys,keys.Length,divisor) == 1;
        public bool Subscribe(uint player, InterestKind kind, ulong[] keys, int[] rates)
            => keys != null && rates != null && rates.Length == keys.Length && ng_subscribe(Handle,player,(int)kind,keys,rates,keys.Length) == 1;
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_threshold_clock(IntPtr game,int enabled);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_validate_layout([MarshalAs(UnmanagedType.LPUTF8Str)] string config);
        public static bool ValidateLayout(string config) => ng_validate_layout(config) == 1;
        public bool UseThresholdClock(bool enabled) => ng_threshold_clock(Handle,enabled?1:0)==1;
        public bool SetPlayerTtl(long milliseconds) => ng_player_ttl(Handle,milliseconds) == 1;
        [UnmanagedFunctionPointer(ABI)]
        delegate int NativeStep(IntPtr context, IntPtr state, int stateSize, IntPtr input, int inputSize, uint tick, IntPtr output, int capacity);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_register_prediction(IntPtr game, ushort schema, ushort version, byte[] neutral, int size, NativeStep step, IntPtr context);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_bind_prediction(IntPtr game, uint id, uint player);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_predict(IntPtr game, uint id, byte[] data, int size);
        readonly List<GCHandle> predictions = new List<GCHandle>();
        bool stepping;
        static readonly NativeStep dispatch = RunStep;
        sealed class PredictionModel {
            public NetcodeGame Game;
            public ushort Schema;
            public Func<object[],byte[],uint,object[]> Step;
        }
        [AOT.MonoPInvokeCallback(typeof(NativeStep))]
        static int RunStep(IntPtr context, IntPtr state, int stateSize, IntPtr input, int inputSize, uint tick, IntPtr output, int capacity) {
            var model = (PredictionModel)GCHandle.FromIntPtr(context).Target;
            if (model.Game.stepping) return 0;
            model.Game.stepping = true;
            try {
                var packed = new byte[stateSize];
                var command = new byte[inputSize];
                Marshal.Copy(state, packed, 0, packed.Length);
                Marshal.Copy(input, command, 0, command.Length);
                var fields = model.Step(model.Game.Decode(model.Schema, packed, packed.Length), command, tick);
                if (fields == null) return 0;
                var result = model.Game.Encode(model.Schema, false, fields);
                if (result.Length > capacity) return 0;
                Marshal.Copy(result, 0, output, result.Length);
                return result.Length;
            } catch (Exception e) { Debug.LogException(e); return 0; }
            finally { model.Game.stepping = false; }
        }
        public bool RegisterPrediction(ushort schema, ushort version, byte[] neutral, Func<object[],byte[],uint,object[]> step) {
            if (step == null || neutral == null) return false;
            var instance = Handle;
            var context = GCHandle.Alloc(new PredictionModel { Game=this, Schema=schema, Step=step });
            predictions.Add(context);
            bool registered = false;
            try {
                registered = ng_register_prediction(instance,schema,version,neutral,neutral.Length,dispatch,GCHandle.ToIntPtr(context)) == 1;
                return registered;
            } finally {
                if (!registered) { predictions.Remove(context); context.Free(); }
            }
        }
        public bool BindPrediction(uint id, uint player) => ng_bind_prediction(Handle,id,player) == 1;
        public bool Predict(uint id, byte[] input) => input != null && ng_predict(Handle,id,input,input.Length) == 1;
        public object[] PredictedFields(uint id) => ReadState(id, 2);
        IntPtr Handle => stepping ? throw new InvalidOperationException("Simulation callbacks must be pure") : handle == IntPtr.Zero ? throw new ObjectDisposedException(nameof(NetcodeGame)) : handle;
        public NetcodeGame(string config, bool host, string address, int port, int capacity = 4, bool spectator = false)
        {
            handle = ng_open(config, host ? 1 : 0, address, port, capacity, spectator ? 1 : 0);
            if (handle == IntPtr.Zero) throw new InvalidOperationException(Marshal.PtrToStringAnsi(ng_error()));
        }
        public void Dispose() {
            if (stepping) throw new InvalidOperationException("Simulation callbacks must be pure");
            if (handle == IntPtr.Zero) return;
            ng_close(handle);
            handle = IntPtr.Zero;
            foreach (var registration in predictions) registration.Free();
            predictions.Clear();
        }
        public bool Start() => ng_start(Handle) == 1;
        public bool Model(ushort schema, ushort[] fields, Ownership policy, uint cooldown = 0)
        {
            if (ng_model(Handle, schema, fields, fields.Length, (int)policy, cooldown) != 1) return false;
            writable[schema] = new HashSet<ushort>(fields);
            return true;
        }
        public GameFrame Advance()
        {
            if (ng_pump(Handle, out var frame) < 0) throw new InvalidOperationException(Marshal.PtrToStringAnsi(ng_error()));
            while (handle != IntPtr.Zero && ng_event(handle, out var e) == 1) Changed?.Invoke(e);
            while (handle != IntPtr.Zero && ng_next_rpc(handle,out uint id,out uint sender,out ushort method,buffer,buffer.Length,out int size)==1) {
                var payload = new byte[size];
                Array.Copy(buffer,payload,size);
                RpcReceived?.Invoke(id,sender,method,payload);
            }
            while (handle != IntPtr.Zero && ng_next_typed(handle,out uint objId,out uint from,out ushort typedId,out ushort argSchema,buffer,buffer.Length,out int byteCount)==1) {
                var args=Decode(argSchema,buffer,byteCount);
                handlers[typedId]?.Invoke(from,objId,args);
            }
            while (handle != IntPtr.Zero && ng_rpc_outcome(handle,out ulong token,out uint player,out uint obj,out ushort eventId,out int status)==1)
                RpcOutcome?.Invoke(token,player,obj,eventId,(RpcStatus)status);
            return frame;
        }
        public bool Claim(uint id) => ng_claim(Handle, id, 0) == 1;
        public bool Release(uint id) => ng_claim(Handle, id, 1) == 1;
        public bool Answer(GameEvent request, bool accept) => ng_answer(Handle, ref request, accept ? 1 : 0) == 1;
        public uint Writer(uint id) => ng_writer(Handle, id);
        public bool Loaded(uint scene, uint generation) => ng_loaded(Handle, scene, generation) == 1;
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_load_map(IntPtr game, uint scene, [MarshalAs(UnmanagedType.LPUTF8Str)] string asset);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_unload_map(IntPtr game, uint scene);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_map_asset(IntPtr game, uint scene, [Out] byte[] data, int capacity);
        [DllImport(Plugin, CallingConvention=ABI)] static extern uint ng_find(IntPtr game, uint scene, ulong key);
        [DllImport(Plugin, CallingConvention=ABI)] static extern uint ng_parent(IntPtr game, uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_reparent(IntPtr game, uint child, uint parent);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_destroy(IntPtr game, uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_allow_spawn(IntPtr game, ushort schema, byte[] data, int size);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_request_spawn(IntPtr game, ushort schema, uint scene, out uint sequence);
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_change_map(IntPtr game, uint scene, [MarshalAs(UnmanagedType.LPUTF8Str)] string asset);
        public bool ChangeMap(uint scene, string asset) => ng_change_map(Handle, scene, asset) == 1;
        public bool LoadMap(uint scene, string asset) => ng_load_map(Handle, scene, asset) == 1;
        public bool UnloadMap(uint scene) => ng_unload_map(Handle, scene) == 1;
        public string MapAsset(uint scene) => Encoding.UTF8.GetString(buffer, 0, ng_map_asset(Handle, scene, buffer, buffer.Length));
        public uint FindObject(uint scene, ulong key) => ng_find(Handle, scene, key);
        public uint ParentOf(uint id) => ng_parent(Handle, id);
        public bool Reparent(uint child, uint parent) => ng_reparent(Handle, child, parent) == 1;
        public bool DestroyObject(uint id) => ng_destroy(Handle, id) == 1;
        public bool AllowSpawn(ushort schema, params object[] defaults) {
            var data = Encode(schema, false, defaults);
            return ng_allow_spawn(Handle, schema, data, data.Length) == 1;
        }
        [DllImport(Plugin, CallingConvention=ABI)] static extern int ng_allow_spawn_policy(IntPtr game, ushort schema, byte[] data, int size, int creator);
        [DllImport(Plugin, CallingConvention=ABI)] static extern ulong ng_request_reference(IntPtr game, ushort schema, uint scene);
        [DllImport(Plugin, CallingConvention=ABI)] static extern uint ng_resolve_reference(IntPtr game, ulong reference);
        public bool AllowCreatorSpawn(ushort schema, params object[] fields) {
            var data = Encode(schema,false,fields);
            return ng_allow_spawn_policy(Handle,schema,data,data.Length,1) == 1;
        }
        public ulong RequestReference(ushort schema,uint scene=0) => ng_request_reference(Handle,schema,scene);
        [DllImport(Plugin, CallingConvention=ABI)] static extern ulong ng_field_reference(IntPtr game,uint id);
        [DllImport(Plugin, CallingConvention=ABI)] static extern uint ng_resolve_field(IntPtr game,ulong reference);
        readonly Dictionary<uint, NetcodeReplica> replicas = new Dictionary<uint, NetcodeReplica>();
        internal bool AttachReplica(uint id, NetcodeReplica replica) {
            if(replicas.TryGetValue(id,out var other) && other && other!=replica) return false;
            replicas[id]=replica;
            return true;
        }
        internal void DetachReplica(uint id, NetcodeReplica replica) {
            if(replicas.TryGetValue(id,out var old) && old==replica) { replicas.Remove(id); UnbindReplica(id); }
        }
        public ulong FieldReference(uint id) => ng_field_reference(handle,id);
        public GameObject ResolveGameObject(ulong reference) {
            uint id=ng_resolve_field(handle,reference);
            return id!=0 && replicas.TryGetValue(id,out var item) && item && item.isActiveAndEnabled ? item.gameObject : null;
        }
        public uint ResolveReference(ulong reference) => ng_resolve_reference(Handle,reference);
        public uint RequestSpawn(ushort schema, uint scene = 0) => ng_request_spawn(Handle, schema, scene, out uint sequence) == 1 ? sequence : 0;
        public uint Spawn(ushort schema, object[] fields, uint scene = 0, ulong key = 0, uint parent = 0, Ownership? mode = null)
        {
            var data = Encode(schema, false, fields);
            return ng_spawn_mode(Handle, schema, data, data.Length, scene, key, parent, mode.HasValue ? (int)mode.Value : -1, out uint id) == 1 ? id : 0;
        }
        public bool Submit(uint id, params object[] fields)
        {
            var schema = ng_schema(Handle, id);
            if (schema == 0) return false;
            var data = Encode(schema, !IsHost, fields);
            return ng_submit(Handle, id, data, data.Length) == 1;
        }
        byte[] Encode(ushort schema, bool subset, object[] fields)
        {
            using (var mem = new MemoryStream())
            using (var w = new BinaryWriter(mem)) {
                int index = 0;
                for (int slot = 0; ng_fields(handle, schema, slot, out int kind, out ushort id) == 1; ++slot) {
                    if (subset && (!writable.TryGetValue(schema, out var mask) || !mask.Contains(id))) continue;
                    if (index == fields.Length) throw new ArgumentException("Missing field");
                    var value = fields[index++];
                    if (value==null) throw new ArgumentException("Null network field");
                    switch (kind) {
                    case 0: w.Write((float)value); break;
                    case 1: w.Write((bool)value); break;
                    case 2: w.Write((int)value); break;
                    case 3: var v2 = (Vector2)value; w.Write(v2.x); w.Write(v2.y); break;
                    case 4: var v3 = (Vector3)value; w.Write(v3.x); w.Write(v3.y); w.Write(v3.z); break;
                    case 5: var q = (Quaternion)value; w.Write(q.x); w.Write(q.y); w.Write(q.z); w.Write(q.w); break;
                    case 6: var s = Encoding.UTF8.GetBytes((string)value); w.Write(checked((ushort)s.Length)); w.Write(s); break;
                    case 7: var a = (int[])value; w.Write(checked((ushort)a.Length)); foreach (var n in a) w.Write(n); break;
                    case 8: var f = (float[])value; w.Write(checked((ushort)f.Length)); foreach (var n in f) w.Write(n); break;
                    case 9: var v4=(Vector4)value; w.Write(v4.x); w.Write(v4.y); w.Write(v4.z); w.Write(v4.w); break;
                    case 10: var c=(Color)value; w.Write(c.r); w.Write(c.g); w.Write(c.b); w.Write(c.a); break;
                    case 11: w.Write((ulong)value); break;
                    case 12: var bytes=(byte[])value; w.Write(checked((ushort)bytes.Length)); w.Write(bytes); break;
                    case 13: var points=(Vector2[])value; w.Write(checked((ushort)points.Length)); foreach(var p in points){w.Write(p.x); w.Write(p.y);} break;
                    case 14: var map=(IDictionary<int,int>)value; w.Write(checked((ushort)map.Count));
                        foreach(var pair in new SortedDictionary<int,int>(map)){w.Write(pair.Key); w.Write(pair.Value);} break;
                    case 15: var list=(LinkedList<int>)value; w.Write(checked((ushort)list.Count)); foreach(var item in list) w.Write(item); break;
                    default: throw new ArgumentException("Unknown field type");
                    }
                }
                if (index != fields.Length) throw new ArgumentException("Unexpected field");
                return mem.ToArray();
            }
        }
        public object[] Read(uint id, bool presentation = true)
            => ReadState(id, presentation ? 1 : 0);
        object[] ReadState(uint id, int mode)
        {
            int count = ng_read(Handle, id, mode, buffer, buffer.Length);
            if (count <= 0) return Array.Empty<object>();
            return Decode(ng_schema(Handle, id), buffer, count);
        }
        public static object ReadField(BinaryReader r,int kind) {
            switch (kind) {
            case 0: return r.ReadSingle();
            case 1: return r.ReadBoolean();
            case 2: return r.ReadInt32();
            case 3: return new Vector2(r.ReadSingle(),r.ReadSingle());
            case 4: return new Vector3(r.ReadSingle(),r.ReadSingle(),r.ReadSingle());
            case 5: return new Quaternion(r.ReadSingle(),r.ReadSingle(),r.ReadSingle(),r.ReadSingle());
            case 6: return Encoding.UTF8.GetString(r.ReadBytes(r.ReadUInt16()));
            case 7: var a = new int[r.ReadUInt16()]; for (int j=0;j<a.Length;j++) a[j]=r.ReadInt32(); return a;
            case 8: var f = new float[r.ReadUInt16()]; for (int j=0;j<f.Length;j++) f[j]=r.ReadSingle(); return f;
            case 9: return new Vector4(r.ReadSingle(),r.ReadSingle(),r.ReadSingle(),r.ReadSingle());
            case 10: return new Color(r.ReadSingle(),r.ReadSingle(),r.ReadSingle(),r.ReadSingle());
            case 11: return r.ReadUInt64();
            case 12: return r.ReadBytes(r.ReadUInt16());
            case 13: var points=new Vector2[r.ReadUInt16()]; for(int j=0;j<points.Length;j++) points[j]=new Vector2(r.ReadSingle(),r.ReadSingle()); return points;
            case 14: var map=new Dictionary<int,int>(); int size=r.ReadUInt16(); for(int j=0;j<size;j++) map.Add(r.ReadInt32(),r.ReadInt32()); return map;
            case 15: var list=new LinkedList<int>(); int length=r.ReadUInt16(); for(int j=0;j<length;j++) list.AddLast(r.ReadInt32()); return list;
            default: throw new InvalidDataException("Unknown field type");
            }
        }
        object[] Decode(ushort schema, byte[] data, int count)
        {
            var fields = new List<object>();
            using (var r = new BinaryReader(new MemoryStream(data, 0, count))) {
                for (int i = 0; ng_fields(handle,schema,i,out int kind,out _) == 1; ++i) {
                    fields.Add(ReadField(r,kind));
                }
                if(r.BaseStream.Position!=count) throw new InvalidDataException("Invalid field payload length");
            }
            return fields.ToArray();
        }
    }
}
