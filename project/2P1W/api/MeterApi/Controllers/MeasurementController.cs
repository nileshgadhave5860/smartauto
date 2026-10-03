using Microsoft.AspNetCore.Mvc;

namespace MeterApi.Controllers;

[ApiController]
[Route("api/[controller]")]
public sealed class MeasurementController(LatestReadingStore store) : ControllerBase
{
    [HttpGet("getMesurment")]
    public IActionResult GetMesurment()
    {
        var reading = store.GetLatest();
        return Ok(new Dictionary<string, object?>
        {
            ["available"] = reading is not null,
            ["LNVolt"] = reading?.L1Voltage,
            ["LNA"] = reading?.L1Current,
            ["receivedAtUtc"] = reading?.ReceivedAtUtc
        });
    }
}