using BeamMPManager.Tests.GameModTests;
using MoonSharp.Interpreter;
using Xunit;

namespace NG64.HarnessTests;

public class ServerPluginTests
{
    private static readonly string PluginPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "server", "NG64", "main.lua"));

    private static BeamMpServerLuaHarness Load()
    {
        var h = new BeamMpServerLuaHarness();
        h.SetConnectedPlayer(1, "Alice");
        h.SetConnectedPlayer(2, "Bob");
        h.SetConnectedPlayer(3, "Cara");
        // the harness only records TriggerClientEventJson; BeamMP's plain-string TriggerClientEvent is shimmed here
        h.Eval("_sent = {} MP.TriggerClientEvent = function(pid, name, data) table.insert(_sent, {pid = pid, name = name, data = data}) return true end");
        h.LoadServerScript(File.ReadAllText(PluginPath));
        return h;
    }

    private static List<(int PlayerId, string Name, string Data)> Sent(BeamMpServerLuaHarness h)
    {
        var list = new List<(int, string, string)>();
        var t = h.Eval("return _sent").Table;
        foreach (var row in t.Values)
            list.Add(((int)row.Table.Get("pid").Number, row.Table.Get("name").String, row.Table.Get("data").String));
        return list;
    }

    [Fact]
    public void RegistersItsEvents()
    {
        var h = Load();
        Assert.True(h.IsEventRegistered("ng64State"));
        Assert.True(h.IsEventRegistered("ng64Gone"));
        Assert.True(h.IsEventRegistered("onPlayerDisconnect"));
    }

    [Fact]
    public void StateIsRelayedToEveryoneButTheSender()
    {
        var h = Load();
        h.TriggerEvent("ng64State", DynValue.NewNumber(1), DynValue.NewString("1.0,2.0,3.0,0.5,205521409,196,21,273"));
        var sent = Sent(h).Where(e => e.Name == "ng64Remote").ToList();
        Assert.Equal(new[] { 2, 3 }, sent.Select(e => e.PlayerId).OrderBy(x => x));
        Assert.All(sent, e => Assert.Equal("1|1.0,2.0,3.0,0.5,205521409,196,21,273", e.Data));
    }

    [Fact]
    public void OversizedOrNonStringPayloadIsDropped()
    {
        var h = Load();
        h.TriggerEvent("ng64State", DynValue.NewNumber(1), DynValue.NewString(new string('9', 300)));
        h.TriggerEvent("ng64State", DynValue.NewNumber(1), DynValue.NewNumber(5));
        Assert.DoesNotContain(Sent(h), e => e.Name == "ng64Remote");
    }

    [Fact]
    public void LeavingPlayersMarioIsRemovedForEveryone()
    {
        var h = Load();
        h.TriggerEvent("onPlayerDisconnect", DynValue.NewNumber(2));
        h.TriggerEvent("ng64Gone", DynValue.NewNumber(3), DynValue.NewString(""));
        var gone = Sent(h).Where(e => e.Name == "ng64RemoteGone").ToList();
        Assert.Contains(gone, e => e.PlayerId == -1 && e.Data == "2");
        Assert.Contains(gone, e => e.PlayerId == -1 && e.Data == "3");
    }
}
