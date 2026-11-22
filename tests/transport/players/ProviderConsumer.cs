using System;
using System.IO;
using System.Linq;
using Netcode;
using UnityEngine;

public sealed class ProviderConsumer : MonoBehaviour
{
    static readonly object[] Fields = {42, 11, 3, 5, 0, 7, -4, 16777219};
    NetcodeGame game;
    bool host, resuming, sent;
    int reported;
    string ticketPath;
    float quitAfter;

    static string Argument(string[] args, string name, string fallback = "")
    {
        int at = Array.IndexOf(args, name);
        return at >= 0 && at + 1 < args.Length ? args[at + 1] : fallback;
    }

    void Start()
    {
        Application.runInBackground = true;
        var args = Environment.GetCommandLineArgs();
        host = args.Contains("--host");
        resuming = args.Contains("--resume");
        ticketPath = Argument(args, "--ticket");
        var expected = Argument(args, "--expect-error");
        try {
            var layout = File.ReadAllText(Path.Combine(Application.streamingAssetsPath, "consumer.cfg"));
            var ticket = resuming ? File.ReadAllBytes(ticketPath) : null;
            var endpoint = Argument(args, "--endpoint");
            var provider = Argument(args, "--provider");
            var module = Argument(args, "--module");
            game = provider.Length == 0
                ? NetcodeGame.OpenWithProvider(layout, host, endpoint, capacity: 2, module: module, resume: ticket)
                : NetcodeGame.OpenWithProvider(layout, host, endpoint, capacity: 2, provider: provider,
                    module: module, resume: ticket);
            if (expected.Length != 0) throw new Exception("Expected open failure");
            if (!game.BindCallable("Proof", 71, 2, RpcDirection.Server, true, (Action<uint, uint, int>)Proof, room: true) ||
                !game.BindCallable("Reply", 72, 2, RpcDirection.Client, true, (Action<uint, uint, int>)Reply, room: true))
                throw new Exception("RPC registration");
            Debug.Log("OPEN sdk=" + NetcodeGame.SdkBuildId + " provider=" + game.TransportBuildId);
        } catch (Exception ex) {
            if (expected.Length != 0 && ex.Message.Contains(expected)) {
                Debug.Log("OPEN_ERROR " + ex.Message);
                enabled = false;
                Application.Quit();
            } else {
                Fail(ex);
            }
        }
    }

    void Fail(Exception ex)
    {
        Debug.LogException(ex);
        enabled = false;
        Application.Quit(1);
    }

    void Proof(uint sender, uint subject, int phase)
    {
        if (!host || sender != 2 || subject != 0 || phase != ++reported)
            throw new Exception("Unexpected proof");
        if (!game.CallTracked("Reply", 0, 2, 0, 42)) throw new Exception("Reply rejected");
        Debug.Log($"HOST_RPC phase={phase} player={sender}");
        if (phase == 2) quitAfter = Time.realtimeSinceStartup + 0.5f;
    }

    void Reply(uint sender, uint subject, int value)
    {
        if (host || !sent || sender != 1 || subject != 0 || value != 42)
            throw new Exception("Unexpected reply");
        if (!resuming) {
            var ticket = game.ResumeTicket();
            if (ticket.Length == 0) throw new Exception("Empty ticket");
            File.WriteAllBytes(ticketPath, ticket);
        }
        Debug.Log("GUEST_PASS fields=8 player=2 resumed=" + (resuming ? 1 : 0));
        enabled = false;
        Application.Quit();
    }

    void Update()
    {
        try {
            var frame = game.Advance();
            if (host) {
                if (frame.Round == 0 && frame.Seats == 2 && (!game.Start() || game.Spawn(1, Fields) != 1))
                    throw new Exception("Cannot start round");
                if (quitAfter > 0 && Time.realtimeSinceStartup >= quitAfter) {
                    Debug.Log("HOST_PASS");
                    Application.Quit();
                }
            } else if (!sent && frame.Round != 0 && frame.Player == 2 && game.Read(1, false).SequenceEqual(Fields)) {
                sent = game.CallMethod("Proof", 0, resuming ? 2 : 1);
            }
        } catch (Exception ex) {
            Fail(ex);
        }
    }

    void OnDestroy() { game?.Dispose(); }
}
