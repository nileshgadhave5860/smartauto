using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Options;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Protocol;

namespace MeterApi;

public sealed class MqttOptions
{
    public string Host { get; set; } = "localhost";
    public int Port { get; set; } = 1883;
    public string Topic { get; set; } = "mfm/readings";
    public string Username { get; set; } = "";
    public string Password { get; set; } = "";
    public string ClientId { get; set; } = "101";
}

public sealed record MeterReading(double L1Voltage, double L1Current, DateTimeOffset ReceivedAtUtc);

public sealed class LatestReadingStore
{
    private readonly object _lock = new();
    private MeterReading? _latest;

    public MeterReading? GetLatest()
    {
        lock (_lock)
        {
            return _latest;
        }
    }

    public void Update(double l1Voltage, double l1Current)
    {
        lock (_lock)
        {
            _latest = new MeterReading(l1Voltage, l1Current, DateTimeOffset.UtcNow);
        }
    }
}

public sealed class MqttTelemetryService(
    IOptions<MqttOptions> options,
    LatestReadingStore readings,
    ILogger<MqttTelemetryService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var settings = options.Value;

        while (!stoppingToken.IsCancellationRequested)
        {
            var client = new MqttFactory().CreateMqttClient();
            client.ApplicationMessageReceivedAsync += message =>
            {
                try
                {
                    var payload = Encoding.UTF8.GetString(message.ApplicationMessage.PayloadSegment);
                    using var document = JsonDocument.Parse(payload);
                    var root = document.RootElement;
                    if (root.TryGetProperty("LNVolt", out var voltage) && voltage.TryGetDouble(out var l1Voltage) &&
                        root.TryGetProperty("LNA", out var current) && current.TryGetDouble(out var l1Current))
                    {
                        readings.Update(l1Voltage, l1Current);
                    }
                    else
                    {
                        logger.LogWarning("Ignoring MQTT message without numeric 'LNVolt' and 'LNA' values");
                    }
                }
                catch (JsonException exception)
                {
                    logger.LogWarning(exception, "Ignoring invalid meter JSON payload");
                }

                return Task.CompletedTask;
            };

            var mqttOptions = new MqttClientOptionsBuilder()
                .WithClientId(settings.ClientId)
                .WithTcpServer(settings.Host, settings.Port);

            if (!string.IsNullOrWhiteSpace(settings.Username))
            {
                mqttOptions.WithCredentials(settings.Username, settings.Password);
            }

            try
            {
                await client.ConnectAsync(mqttOptions.Build(), stoppingToken);
                await client.SubscribeAsync(
                    new MqttTopicFilterBuilder()
                        .WithTopic(settings.Topic)
                        .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                        .Build(),
                    stoppingToken);

                logger.LogInformation("Subscribed to MQTT topic {Topic} on {Host}:{Port}",
                    settings.Topic, settings.Host, settings.Port);

                while (client.IsConnected && !stoppingToken.IsCancellationRequested)
                {
                    await Task.Delay(TimeSpan.FromSeconds(1), stoppingToken);
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception exception)
            {
                logger.LogWarning(exception, "MQTT connection failed; retrying in 5 seconds");
            }

            if (!stoppingToken.IsCancellationRequested)
            {
                await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken);
            }
        }
    }
}