using Api.Module;
using Api.Services;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.ModelBinding;

namespace Api.Controllers;

[ApiController]
[Route("api/[controller]")]
public class DeviceController : ControllerBase
{
	private readonly MqttService _mqttService;

	public DeviceController(MqttService mqttService)
	{
		_mqttService = mqttService;
	}

	[HttpPut("limit")]
	public async Task<IActionResult> SetLimit([FromQuery, BindRequired] int deviceId, [FromBody] DeviceLimit request)
	{
		await _mqttService.PublishLimitAsync(deviceId, request);
		return NoContent();
	}

	[HttpPut("auto/status")]
	[ProducesResponseType(StatusCodes.Status204NoContent)]
	[ProducesResponseType(StatusCodes.Status400BadRequest)]
	public async Task<IActionResult> SetAutoStatus(
		[FromQuery, BindRequired] int autoId,
		[FromQuery, BindRequired] int status)
	{
		if (autoId <= 0)
		{
			return BadRequest("autoId must be greater than zero.");
		}

		if (status is not (1 or 2))
		{
			return BadRequest("status must be 1 (On) or 2 (Off).");
		}

		await _mqttService.PublishAutoStatusAsync(autoId, status);
		return NoContent();
	}

	[HttpGet("online")]
	[ProducesResponseType(typeof(bool), StatusCodes.Status200OK)]
	[ProducesResponseType(StatusCodes.Status400BadRequest)]
	public async Task<ActionResult<bool>> GetDeviceOnline(
		[FromQuery, BindRequired] int deviceId,
		CancellationToken cancellationToken)
	{
		if (deviceId <= 0)
		{
			return BadRequest("deviceId must be greater than zero.");
		}

		var isOnline = await _mqttService.GetDeviceOnlineAsync(deviceId, cancellationToken);
		return Ok(isOnline);
	}

	[HttpGet("auto/status")]
	[ProducesResponseType(typeof(int), StatusCodes.Status200OK)]
	[ProducesResponseType(StatusCodes.Status404NotFound)]
	[ProducesResponseType(StatusCodes.Status400BadRequest)]
	public async Task<ActionResult<int>> GetAutoStatus(
		[FromQuery, BindRequired] int autoId,
		CancellationToken cancellationToken)
	{
		if (autoId <= 0)
		{
			return BadRequest("autoId must be greater than zero.");
		}

		var status = await _mqttService.GetAutoStatusAsync(autoId, cancellationToken);
		if (status is null)
		{
			return NotFound();
		}

		return Ok(status.Value);
	}

	[HttpGet("motor/status")]
	[ProducesResponseType(typeof(MotorStatusResponse), StatusCodes.Status200OK)]
	[ProducesResponseType(StatusCodes.Status400BadRequest)]
	public async Task<ActionResult<MotorStatusResponse>> GetMotorStatus(
		[FromQuery, BindRequired] int autoId,
		CancellationToken cancellationToken)
	{
		if (autoId <= 0)
		{
			return BadRequest("autoId must be greater than zero.");
		}

		var motorStatus = await _mqttService.GetMotorStatusAsync(autoId, cancellationToken);
		if (motorStatus is null)
		{
			return Ok(new MotorStatusResponse(autoId, false, "not setup auto"));
		}

		var isMotorOn = motorStatus == 1;
		var reason = isMotorOn ? "motor is on" : "motor is off";
		return Ok(new MotorStatusResponse(autoId, isMotorOn, reason));
	}

	[HttpGet("limit")]
	public async Task<ActionResult<DeviceLimit>> GetLimit([FromQuery, BindRequired] int deviceId, CancellationToken cancellationToken)
	{
		var limit = await _mqttService.GetLimitAsync(deviceId, cancellationToken);
		if (limit is null)
		{
			return NotFound();
		}

		return Ok(limit);
	}

	[HttpGet("measurements")]
	[ProducesResponseType(typeof(List<DevicePhaseReading>), StatusCodes.Status200OK)]
	[ProducesResponseType(StatusCodes.Status400BadRequest)]
	public async Task<ActionResult<List<DevicePhaseReading>>> GetMeasurements(
		[FromQuery, BindRequired] int deviceId,
		CancellationToken cancellationToken)
	{
		if (deviceId <= 0)
		{
			return BadRequest("deviceId must be greater than zero.");
		}

		var readings = await _mqttService.GetPhaseReadingsAsync(deviceId, cancellationToken);
		return Ok(readings ?? new List<DevicePhaseReading>());
	}
}
