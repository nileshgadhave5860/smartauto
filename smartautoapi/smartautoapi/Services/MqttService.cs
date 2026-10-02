using MQTTnet;
using smartautoapi.DTO;
using System.Text;
using System.Text.Json;

namespace SmartAutoApi.Services
{
public class MqttService
{
    private readonly IMqttClient _client;
    private readonly IConfiguration _configuration;

    public MqttService(IConfiguration configuration)
    {
        _configuration = configuration;

        var factory = new MqttClientFactory();
        _client = factory.CreateMqttClient();
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
    public async Task PublishLimitAsync(SetLimitRequest request)
        {
            if (!_client.IsConnected)
            {

                var options = new MqttClientOptionsBuilder()
                    .WithTcpServer(
                        _configuration["Mqtt:Host"],
                        int.Parse(_configuration["Mqtt:Port"]!))
                    .Build();

                await _client.ConnectAsync(options);
            }

            string topic = $"smartauto/device/{request.Device}/limit";

            string payload = JsonSerializer.Serialize(request, new JsonSerializerOptions
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




        public async Task DisconnectAsync()
    {
        if (_client.IsConnected)
        {
            await _client.DisconnectAsync();
        }
    }
}
}