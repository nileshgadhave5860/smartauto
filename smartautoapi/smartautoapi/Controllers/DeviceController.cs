using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using smartautoapi.DTO;
using SmartAutoApi.Services;

namespace smartautoapi.Controllers
{
    [ApiController]
    [Route("api/device")]
    public class DeviceController : ControllerBase
    {
        private readonly MqttService _mqttService;

        public DeviceController(MqttService mqttService)
        {
            _mqttService = mqttService;
        }

        [HttpGet("{id}/{status}")]
        public async Task<IActionResult> Send(
            int id,
            int status)
        {
            await _mqttService.PublishAsync(
                $"smartauto/device/{id}",
                status.ToString());

            return Ok();
        }
        [HttpPost("set-limit")]
        public async Task<IActionResult> SetLimit(
       [FromBody] SetLimitRequest request)
        {
            // Save to database here

            // Send configuration to ESP32
            await _mqttService.PublishLimitAsync(request);

            return Ok(new
            {
                success = true,
                message = "Limit saved and sent to device",
                device = request.Device
            });
        }
    }
}
