using MQTTnet;
using Api.Module;
using System.Buffers;
using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Hosting;

namespace Api.Services
{
public class MqttService : BackgroundService
{
    private readonly IMqttClient _client;
    private readonly IConfiguration _configuration;
    private readonly ILogger<MqttService> _logger;
    private readonly ConcurrentDictionary<int, List<DevicePhaseReading>> _measurements = new();
    private readonly ConcurrentDictionary<int, DeviceLimit> _limits = new();
    private readonly ConcurrentDictionary<int, int> _autoStatuses = new();
    private readonly ConcurrentDictionary<int, (bool IsOnline, DateTimeOffset UpdatedAt)> _deviceAvailability = new();
    private static readonly TimeSpan AvailabilityHeartbeatTimeout = TimeSpan.FromSeconds(65);

    public MqttService(IConfiguration configuration, ILogger<MqttService> logger)
    {
        _configuration = configuration;
        _logger = logger;

        var factory = new MqttClientFactory();
        _client = factory.CreateMqttClient();
        _client.ApplicationMessageReceivedAsync += HandleAutomationMessageAsync;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMinutes(1));
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                if (!_client.IsConnected)
                {
                    await ConnectAsync();
                    var options = new MqttClientSubscribeOptionsBuilder()
                        .WithTopicFilter(filter => filter
                            .WithTopic("smartauto/device/+/+/#")
                            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce))
                        .Build();
                    await _client.SubscribeAsync(options, stoppingToken);
                }

                if (!await timer.WaitForNextTickAsync(stoppingToken))
                {
                    break;
                }

                foreach (var measurement in _measurements)
                {
                    await PublishCurrentAsync(measurement.Key, measurement.Value);
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception exception)
            {
                _logger.LogWarning(exception, "MQTT automation temporarily failed; retrying.");
                await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken);
            }
        }
    }

    private async Task HandleAutomationMessageAsync(MqttApplicationMessageReceivedEventArgs args)
    {
        var topicParts = args.ApplicationMessage.Topic.Split('/');
        if (topicParts.Length < 4
            || topicParts[0] != "smartauto"
            || topicParts[1] != "device"
            || !int.TryParse(topicParts[2], out var deviceId)
            || args.ApplicationMessage.Payload.IsEmpty)
        {
            return;
        }

        var payload = Encoding.UTF8.GetString(args.ApplicationMessage.Payload.ToArray());
        if (topicParts.Length == 4 && topicParts[3] == "availability")
        {
            if (payload.Trim().Equals("online", StringComparison.OrdinalIgnoreCase))
            {
                _deviceAvailability[deviceId] = (true, DateTimeOffset.UtcNow);
            }
            else if (payload.Trim().Equals("offline", StringComparison.OrdinalIgnoreCase))
            {
                _deviceAvailability[deviceId] = (false, DateTimeOffset.UtcNow);
            }

            return;
        }

        try
        {
            if (topicParts.Length == 4 && topicParts[3] == "measurement")
            {
                var readings = JsonSerializer.Deserialize<List<DevicePhaseReading>>(payload,
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                if (readings is not null)
                {
                    _measurements[deviceId] = readings;
                    await EvaluateMotorAsync(deviceId);
                }
            }
            else if (topicParts.Length == 4 && topicParts[3] == "limit")
            {
                var limit = JsonSerializer.Deserialize<DeviceLimit>(payload,
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                if (limit is not null)
                {
                    _limits[deviceId] = limit;
                    await EvaluateMotorAsync(deviceId);
                }
            }
            else if (topicParts.Length == 5
                && topicParts[3] == "auto"
                && topicParts[4] == "status"
                && int.TryParse(payload, out var status)
                && status is 1 or 2)
            {
                _autoStatuses[deviceId] = status;
                await EvaluateMotorAsync(deviceId);
            }
        }
        catch (JsonException)
        {
            // Ignore malformed retained device messages and keep the last valid state.
        }
    }

    private async Task EvaluateMotorAsync(int deviceId)
    {
        if (!_autoStatuses.TryGetValue(deviceId, out var autoStatus))
        {
            return;
        }

        var isMotorOn = autoStatus == 1;
        var reason = "auto status is off";
        if (isMotorOn)
        {
            isMotorOn = CheckPhases(deviceId, out reason);
        }

        await PublishRetainedAsync($"smartauto/device/{deviceId}/status", isMotorOn ? "1" : "2");
        await PublishRetainedAsync($"smartauto/device/{deviceId}/status/reason", reason);
    }

    private bool CheckPhases(int deviceId, out string reason)
    {
        if (!_limits.TryGetValue(deviceId, out var limit))
        {
            reason = "phase limits are unavailable";
            return false;
        }

        var enabledPhaseLimits = GetPhaseLimits(limit)
            .Where(phase => phase.Limit.IsVol || phase.Limit.IsA)
            .ToList();
        if (enabledPhaseLimits.Count == 0)
        {
            reason = "auto is on; no phase limits are enabled";
            return true;
        }

        if (!_measurements.TryGetValue(deviceId, out var readings))
        {
            reason = "measurements are unavailable";
            return false;
        }

        foreach (var (phaseName, phaseLimit) in enabledPhaseLimits)
        {
            var reading = readings.LastOrDefault(item =>
                string.Equals(item.VName, phaseName, StringComparison.OrdinalIgnoreCase));
            if (reading is null)
            {
                reason = $"{phaseName} measurement is unavailable";
                return false;
            }

            if (phaseLimit.IsVol && (reading.VValue < phaseLimit.MinVol || reading.VValue > phaseLimit.MaxVol))
            {
                reason = $"{phaseName} voltage is outside the configured limits";
                return false;
            }

            if (phaseLimit.IsA && (reading.AValue < phaseLimit.MinA || reading.AValue > phaseLimit.MaxA))
            {
                reason = $"{phaseName} current is outside the configured limits";
                return false;
            }
        }

        reason = "phase limits are within range";
        return true;
    }

    private static IEnumerable<(string Name, PhaseLimit Limit)> GetPhaseLimits(DeviceLimit limit)
    {
        yield return ("N", limit.N);
        yield return ("L", limit.L);
        yield return ("R", limit.R);
        yield return ("B", limit.B);
        yield return ("Y", limit.Y);
    }

    private async Task PublishCurrentAsync(int deviceId, List<DevicePhaseReading> readings)
    {
        var payload = JsonSerializer.Serialize(readings);
        await PublishRetainedAsync($"smartauto/device/{deviceId}/current", payload);
    }

    private async Task PublishRetainedAsync(string topic, string payload)
    {
        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(payload)
            .WithRetainFlag(true)
            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();
        await _client.PublishAsync(message);
    }

    public async Task ConnectAsync()
    {
        if (_client.IsConnected)
            return;

        var options = new MqttClientOptionsBuilder()
            .WithTcpServer(
                _configuration["Mqtt:Host"],
                int.Parse(_configuration["Mqtt:Port"]!))
            .Build();

        await _client.ConnectAsync(options);
    }

    public async Task PublishAsync(string topic, string payload)
    {
        if (!_client.IsConnected)
        {
            await ConnectAsync();
        }

        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(payload)
            .Build();

        await _client.PublishAsync(message);
    }

    public async Task PublishAutoStatusAsync(int autoId, int status)
    {
        await ConnectAsync();

        var topic = $"smartauto/device/{autoId}/auto/status";
        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(status.ToString())
            .WithRetainFlag(true)
            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();

        await _client.PublishAsync(message);
    }
    
    public async Task PublishLimitAsync(int deviceId, DeviceLimit request)
    {
        await ConnectAsync();

        var topic = $"smartauto/device/{deviceId}/limit";
        var payload = JsonSerializer.Serialize(request, new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        });

        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(Encoding.UTF8.GetBytes(payload))
            .WithRetainFlag(true)
            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();

        await _client.PublishAsync(message);
    }

    public Task<DeviceLimit?> GetLimitAsync(int deviceId, CancellationToken cancellationToken = default)
    {
        return GetRetainedJsonAsync<DeviceLimit>($"smartauto/device/{deviceId}/limit", cancellationToken);
    }

    public async Task<List<DevicePhaseReading>?> GetPhaseReadingsAsync(
        int deviceId,
        CancellationToken cancellationToken = default)
    {
        var readings = await GetRetainedJsonAsync<List<DevicePhaseReading>>(
            $"smartauto/device/{deviceId}/measurement",
            cancellationToken);

        return readings?
            .GroupBy(reading => reading.VName, StringComparer.OrdinalIgnoreCase)
            .Select(group => group.Last())
            .ToList();
    }

    private async Task<T?> GetRetainedJsonAsync<T>(string topic, CancellationToken cancellationToken)
        where T : class
    {
        await ConnectAsync();

        var result = new TaskCompletionSource<T?>(TaskCreationOptions.RunContinuationsAsynchronously);
        Func<MqttApplicationMessageReceivedEventArgs, Task> messageHandler = args =>
        {
            if (args.ApplicationMessage.Topic == topic)
            {
                var payload = args.ApplicationMessage.Payload;
                if (payload.IsEmpty)
                {
                    result.TrySetResult(null);
                }
                else
                {
                    try
                    {
                        var json = Encoding.UTF8.GetString(payload.ToArray());
                        var value = JsonSerializer.Deserialize<T>(json, new JsonSerializerOptions
                        {
                            PropertyNameCaseInsensitive = true
                        });
                        result.TrySetResult(value);
                    }
                    catch (JsonException)
                    {
                        result.TrySetResult(null);
                    }
                }
            }

            return Task.CompletedTask;
        };

        _client.ApplicationMessageReceivedAsync += messageHandler;
        try
        {
            var options = new MqttClientSubscribeOptionsBuilder()
                .WithTopicFilter(filter => filter
                    .WithTopic(topic)
                    .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce))
                .Build();

            await _client.SubscribeAsync(options, cancellationToken);
            var timeoutTask = Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
            var completedTask = await Task.WhenAny(result.Task, timeoutTask);
            if (completedTask == result.Task)
            {
                return await result.Task;
            }

            cancellationToken.ThrowIfCancellationRequested();
            return null;
        }
        finally
        {
            _client.ApplicationMessageReceivedAsync -= messageHandler;
            var unsubscribeOptions = new MqttClientUnsubscribeOptionsBuilder()
                .WithTopicFilter(topic)
                .Build();
            await _client.UnsubscribeAsync(unsubscribeOptions, cancellationToken);
        }
    }

    public Task<int?> GetAutoStatusAsync(int autoId, CancellationToken cancellationToken = default)
    {
        return GetRetainedStatusAsync($"smartauto/device/{autoId}/auto/status", cancellationToken);
    }

    public Task<int?> GetMotorStatusAsync(int autoId, CancellationToken cancellationToken = default)
    {
        return GetRetainedStatusAsync($"smartauto/device/{autoId}/status", cancellationToken);
    }

    public Task<bool> GetDeviceOnlineAsync(int deviceId, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var isOnline = _deviceAvailability.TryGetValue(deviceId, out var state)
            && state.IsOnline
            && DateTimeOffset.UtcNow - state.UpdatedAt <= AvailabilityHeartbeatTimeout;
        return Task.FromResult(isOnline);
    }

    private async Task<int?> GetRetainedStatusAsync(string topic, CancellationToken cancellationToken)
    {
        await ConnectAsync();

        var result = new TaskCompletionSource<int?>(TaskCreationOptions.RunContinuationsAsynchronously);
        Func<MqttApplicationMessageReceivedEventArgs, Task> messageHandler = args =>
        {
            if (args.ApplicationMessage.Topic == topic)
            {
                var payload = args.ApplicationMessage.Payload;
                if (payload.IsEmpty)
                {
                    result.TrySetResult(null);
                }
                else if (int.TryParse(Encoding.UTF8.GetString(payload.ToArray()), out var status)
                    && status is 1 or 2)
                {
                    result.TrySetResult(status);
                }
                else
                {
                    result.TrySetResult(null);
                }
            }

            return Task.CompletedTask;
        };

        _client.ApplicationMessageReceivedAsync += messageHandler;
        try
        {
            var options = new MqttClientSubscribeOptionsBuilder()
                .WithTopicFilter(filter => filter
                    .WithTopic(topic)
                    .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce))
                .Build();

            await _client.SubscribeAsync(options, cancellationToken);
            var timeoutTask = Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
            var completedTask = await Task.WhenAny(result.Task, timeoutTask);
            if (completedTask == result.Task)
            {
                return await result.Task;
            }

            cancellationToken.ThrowIfCancellationRequested();
            return null;
        }
        finally
        {
            _client.ApplicationMessageReceivedAsync -= messageHandler;
            var unsubscribeOptions = new MqttClientUnsubscribeOptionsBuilder()
                .WithTopicFilter(topic)
                .Build();
            await _client.UnsubscribeAsync(unsubscribeOptions, cancellationToken);
        }
    }

    public async Task DisconnectAsync()
    {
        if (_client.IsConnected)
        {
            await _client.DisconnectAsync();
        }
    }
}
}